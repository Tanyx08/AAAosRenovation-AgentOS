#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"

static int failures;

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

static void
append_str(char *dst, int *pos, const char *src, int max)
{
  while(*src && *pos < max - 1)
    dst[(*pos)++] = *src++;
  dst[*pos] = 0;
}

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

static int
call_tool(const char *tool, const char *params, struct agent_tool_response *resp)
{
  struct agent_tool_request req;

  memset(&req, 0, sizeof(req));
  strcpy(req.tool, tool);
  strcpy(req.params, params);
  return tool_call(&req, resp);
}

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
  check((reason & AGENT_EVENT_HEARTBEAT) != 0, "heartbeat wakes agent_wait");
  check(event.tick >= start + 5, "heartbeat wait really slept");
}

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
  check(reason == AGENT_EVENT_MESSAGE, "message wakes without heartbeat");
  check(event.tick >= start + 8, "message wait slept until sender fired");
  check(strcmp(event.message, "msg-only") == 0, "message payload delivered");
  check(wait(&status) == child && status == 0, "message sender child exits cleanly");
  check(agent_unwatch(AGENT_WATCH_MESSAGE) == 0, "unwatch message event");
}

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
  if((reason & AGENT_EVENT_HEARTBEAT) == 0)
    exit(1);
  if(agent_heartbeat_stop() < 0)
    exit(1);

  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  if((reason & AGENT_EVENT_MESSAGE) == 0)
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
  multi_agent_test();

  if(failures){
    printf("agentlooptest: %d failures\n", failures);
    exit(1);
  }
  printf("agentlooptest: all tests passed\n");
  exit(0);
}
