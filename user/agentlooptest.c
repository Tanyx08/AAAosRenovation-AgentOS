#include "kernel/types.h"
#include "kernel/agent.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static int failures;

/*
  * agentlooptest.c: 测试 Agent Loop 相关功能，并非现实调用模拟。
  *
  * 本测试覆盖 Agent Loop 的核心功能，包括：
  * - 心跳事件：设置心跳后应按预期间隔唤醒。
  * - 消息事件：关闭心跳后应能通过消息唤醒。
  * - 文件修改事件：watch 一个文件，修改后应能唤醒。
  * - 多 Agent 并发：多个 Worker 同时等待，分别接收消息并稳定退出。
  * - 调度策略：设置不同优先级/配额的 Worker，比较 CPU 获得量。
  *
  * 测试通过后会打印 "all tests passed"，否则会累计失败数并打印。
  */


// 统一断言输出：成功打印 ok，失败打印 FAIL 并累计失败计数。
static void
check(int ok, const char *msg)
{
  if(ok){
    printf("agentlooptest: ok: %s\n", msg);
  } else {
    printf("agentlooptest: FAIL: %s\n", msg);
    failures++;
  }
}

// 向固定大小字符串缓冲区尾部追加文本，避免手写 snprintf。
static void
append_str(char *dst, int *pos, const char *src, int max)
{
  while(*src && *pos < max - 1)
    dst[(*pos)++] = *src++;
  dst[*pos] = 0;
}

// 把无符号整数转成十进制字符串并追加到缓冲区中。
static void
append_uint(char *dst, int *pos, uint64 value, int max)
{
  char tmp[24];
  int n = 0;

  if(value == 0){
    append_str(dst, pos, "0", max);
    return;
  }
  while(value > 0 && n < sizeof(tmp)){
    tmp[n++] = '0' + value % 10;
    value /= 10;
  }
  while(n > 0 && *pos < max - 1)
    dst[(*pos)++] = tmp[--n];
  dst[*pos] = 0;
}

// 封装一次结构化 tool_call，便于测试里复用。
static int
call_tool(const char *tool, const char *params, struct agent_tool_response *resp)
{
  struct agent_tool_request req;

  memset(&req, 0, sizeof(req));
  strcpy(req.tool, tool);
  strcpy(req.params, params);
  return tool_call(&req, resp);
}

// 通过 send_message 工具向指定 pid 的 Agent 发送一条消息。
static int
send_message_to(int pid, const char *message)
{
  struct agent_tool_response resp;
  char params[AGENT_TOOL_PARAM_MAX];
  int pos = 0;

  memset(params, 0, sizeof(params));
  append_str(params, &pos, "target_pid=", sizeof(params));
  append_uint(params, &pos, pid, sizeof(params));
  append_str(params, &pos, ";message=", sizeof(params));
  append_str(params, &pos, message, sizeof(params));
  return call_tool("send_message", params, &resp);
}

// 测试心跳机制：设置心跳后进入 agent_wait，应被心跳事件唤醒。
static void
heartbeat_test(void)
{
  struct agent_wait_event event;
  uint64 start;
  int reason;

  check(agent_heartbeat_set(5) == 0, "heartbeat_set");
  start = uptime();
  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  check(reason == AGENT_WAIT_HEARTBEAT, "heartbeat wakes agent_wait");
  check(event.tick >= start + 5, "heartbeat wait really slept");
}

// 测试纯消息事件：关闭心跳，只保留消息 watch，验证 Agent 会因消息唤醒。
static void
message_only_test(void)
{
  struct agent_wait_event event;
  uint64 start;
  int parent_pid = getpid();
  int child;
  int status = -1;
  int reason;

  check(agent_watch(AGENT_WATCH_MESSAGE) == 0, "watch message event");
  check(agent_heartbeat_stop() == 0, "heartbeat_stop");

  child = fork();
  if(child == 0){
    agent_create(AGENT_TYPE_WORKER, 0, 256);
    sleep(8);
    if(send_message_to(parent_pid, "msg-only") != AGENT_TOOL_OK)
      exit(1);
    exit(0);
  }

  start = uptime();
  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  check(reason == AGENT_WAIT_MESSAGE, "message wakes without heartbeat");
  check(event.tick >= start + 8, "message wait slept until sender fired");
  check(strcmp(event.message, "msg-only") == 0, "message payload delivered");
  check(wait(&status) == child && status == 0, "message sender child exits cleanly");
  check(agent_unwatch(AGENT_WATCH_MESSAGE) == 0, "unwatch message event");
}

// 测试文件修改事件：watch 一个真实文件，子进程写该文件后应唤醒等待中的 Agent。
static void
file_modify_event_test(void)
{
  struct agent_wait_event event;
  uint64 start;
  int child;
  int status = -1;
  int fd;
  int reason;

  fd = open("watchlog", O_CREATE | O_RDWR);
  check(fd >= 0, "create watched file");
  if(fd >= 0)
    close(fd);

  check(agent_watch_file("watchlog") == 0, "watch file modification");
  check(agent_heartbeat_stop() == 0, "heartbeat_stop before file wait");

  child = fork();
  if(child == 0){
    int wfd = open("watchlog", O_RDWR);

    if(wfd < 0)
      exit(1);
    sleep(8);
    if(write(wfd, "Z", 1) != 1){
      close(wfd);
      exit(1);
    }
    close(wfd);
    exit(0);
  }

  start = uptime();
  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  check(reason == AGENT_WAIT_FILEMOD, "file modification wakes agent_wait");
  check(event.tick >= start + 8, "file wait slept until writer fired");
  check(strcmp(event.file, "watchlog") == 0, "file event path delivered");
  check(wait(&status) == child && status == 0, "file writer child exits cleanly");
  check(agent_unwatch(AGENT_WATCH_FILEMOD) == 0, "unwatch file event");
}

// 构造一个完整 Worker Agent Loop：
// 先被心跳唤醒，再被消息唤醒，最后显式声明任务完成。
static void
worker_loop(int interval, const char *expect_message)
{
  struct agent_wait_event event;
  struct agent_info info;
  int reason;

  agent_create(AGENT_TYPE_WORKER, interval, 256);
  if(agent_watch(AGENT_WATCH_MESSAGE) < 0)
    exit(1);
  if(agent_heartbeat_set(interval) < 0)
    exit(1);

  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  if(reason != AGENT_WAIT_HEARTBEAT)
    exit(1);
  if(agent_heartbeat_stop() < 0)
    exit(1);

  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  if(reason != AGENT_WAIT_MESSAGE)
    exit(1);
  if(strcmp(event.message, expect_message) != 0)
    exit(1);

  if(agent_info(&info) < 0 || info.loop_state != AGENT_LOOP_READY)
    exit(1);
  if(agent_wait(0, 0) < 0)
    exit(1);
  if(agent_info(&info) < 0 || info.loop_state != AGENT_LOOP_DONE)
    exit(1);
  exit(0);
}

// 测试多 Agent 并发：两个 Worker 同时等待，各自接收独立消息并稳定退出。
static void
multi_agent_test(void)
{
  int child1;
  int child2;
  int status = -1;

  child1 = fork();
  if(child1 == 0)
    worker_loop(4, "fanout-1");

  child2 = fork();
  if(child2 == 0)
    worker_loop(6, "fanout-2");

  sleep(8);
  check(send_message_to(child1, "fanout-1") == AGENT_TOOL_OK,
        "send_message to worker 1");
  check(send_message_to(child2, "fanout-2") == AGENT_TOOL_OK,
        "send_message to worker 2");
  check(wait(&status) > 0 && status == 0, "first worker exits cleanly");
  check(wait(&status) > 0 && status == 0, "second worker exits cleanly");
}

// 验证普通消息只能占用六个槽位，溢出返回 BUSY，已有消息保持 FIFO。
static void
mailbox_capacity_test(void)
{
  int child = fork();
  int status = -1;

  if(child == 0){
    struct agent_wait_event event;
    uint64 previous = 0;

    agent_create(AGENT_TYPE_WORKER, 0, 256);
    agent_watch(AGENT_WATCH_MESSAGE);
    sleep(12);
    for(int i = 0; i < AGENT_MAILBOX_CAP - AGENT_MAILBOX_SYSTEM_SLOTS; i++){
      memset(&event, 0, sizeof(event));
      if(agent_wait(1, &event) != AGENT_WAIT_MESSAGE ||
         event.sequence <= previous)
        exit(1);
      previous = event.sequence;
    }
    exit(0);
  }

  sleep(2);
  for(int i = 0; i < AGENT_MAILBOX_CAP - AGENT_MAILBOX_SYSTEM_SLOTS; i++)
    check(send_message_to(child, "fifo") == AGENT_TOOL_OK,
          "mailbox accepts FIFO message");
  check(send_message_to(child, "overflow") == AGENT_TOOL_ERR_BUSY,
        "mailbox overflow returns BUSY");
  check(wait(&status) == child && status == 0,
        "mailbox preserves queued FIFO messages");
}

// 调度测试用 Worker：设置 priority/quota 后持续运行一段时间，
// 最后把本轮累计的计数写回父进程，用于比较 CPU 获得量。
static void
sched_worker(int write_fd, int priority, int quota, int runtime_ticks)
{
  uint64 start = uptime();
  uint64 counter = 0;

  agent_create(AGENT_TYPE_WORKER, 0, 256);
  if(agent_sched_set(priority, quota) < 0)
    exit(1);

  while(uptime() - start < runtime_ticks){
    counter++;
    if((counter & 0x3ff) == 0)
      ;
  }
  if(write(write_fd, &counter, sizeof(counter)) != sizeof(counter))
    exit(1);
  exit(0);
}

// 测试 Agent 优先级/配额调度：
// 同时运行高优先级高配额和低优先级低配额 Worker，比较谁拿到更多 CPU。
static void
scheduler_policy_test(void)
{
  struct agent_info info;
  int high_pipe[2];
  int low_pipe[2];
  int high_child;
  int low_child;
  int status = -1;
  uint64 high_count = 0;
  uint64 low_count = 0;

  check(agent_sched_set(6, 4) == 0, "set primary agent scheduling");
  check(agent_info(&info) == 0 &&
        info.sched_priority == 6 &&
        info.sched_quota == 4,
        "agent_info reports scheduling policy");

  check(pipe(high_pipe) == 0, "create high-priority pipe");
  check(pipe(low_pipe) == 0, "create low-priority pipe");

  high_child = fork();
  if(high_child == 0){
    close(high_pipe[0]);
    close(low_pipe[0]);
    close(low_pipe[1]);
    sched_worker(high_pipe[1], 7, 6, 80);
  }

  low_child = fork();
  if(low_child == 0){
    close(low_pipe[0]);
    close(high_pipe[0]);
    close(high_pipe[1]);
    sched_worker(low_pipe[1], 2, 1, 80);
  }

  close(high_pipe[1]);
  close(low_pipe[1]);
  check(read(high_pipe[0], &high_count, sizeof(high_count)) == sizeof(high_count),
        "read high-priority worker count");
  check(read(low_pipe[0], &low_count, sizeof(low_count)) == sizeof(low_count),
        "read low-priority worker count");
  close(high_pipe[0]);
  close(low_pipe[0]);

  check(wait(&status) > 0 && status == 0, "high-priority worker exits cleanly");
  check(wait(&status) > 0 && status == 0, "low-priority worker exits cleanly");
  check(high_count > low_count, "higher priority/quota agent gets more CPU");
}

// 主测试入口：先把当前进程创建为主 Agent，再依次覆盖任务五的各项能力。
int
main(void)
{
  struct agent_info info;

  check((uint64)agent_create(AGENT_TYPE_PRIMARY, 3, 512) > 0,
        "agent_create for loop test");
  check(agent_info(&info) == 0 && info.agent_type == AGENT_TYPE_PRIMARY,
        "agent_info after create");

  heartbeat_test();
  message_only_test();
  file_modify_event_test();
  multi_agent_test();
  mailbox_capacity_test();
  scheduler_policy_test();

  if(failures){
    printf("agentlooptest: %d failures\n", failures);
    exit(1);
  }
  printf("agentlooptest: all tests passed\n");
  exit(0);
}
