#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

static int failures;

static int
contains(const char *s, const char *needle)
{
  int n = strlen(needle);
  int i;

  if(n == 0)
    return 1;
  for(; *s; s++){
    for(i = 0; i < n && s[i] == needle[i]; i++)
      ;
    if(i == n)
      return 1;
  }
  return 0;
}

static void
check(int ok, const char *msg)
{
  if(ok){
    printf("agentinnovationtest: ok: %s\n", msg);
  } else {
    printf("agentinnovationtest: FAIL: %s\n", msg);
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

static void
read_exact(int fd, char *buf, int n)
{
  int got = 0;
  int r;

  while(got < n){
    r = read(fd, buf + got, n - got);
    if(r <= 0)
      break;
    got += r;
  }
}

static void
make_file(const char *path, const char *data)
{
  int fd = open(path, O_CREATE | O_RDWR);

  check(fd >= 0, "create demo file");
  if(fd < 0)
    return;
  write(fd, data, strlen(data));
  close(fd);
}

static int
set_attr_raw(const char *params)
{
  struct agent_tool_response resp;

  return call_tool("set_file_attr", params, &resp);
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

/*
  * Test that shared query cache is populated by one agent and reused by another,
  * and that cache is invalidated when metadata version changes.
  * 第一个 Agent 先查询一次，期望 cache_hit=0，然后 fork 出一个子 Agent，子 Agent 查询同样的内容，期望 cache_hit=1，
  * 最后修改文件属性使得元数据版本改变，再次查询期望 cache_hit=0。
  */
static void
shared_cache_test(void)
{
  struct agent_tool_response resp;
  int child;
  int status = -1;
  const char *query =
    "type=memory;owner=system;tags=shared;public=true;keyword=shared";

  make_file("agentpub", "shared public memory for query cache");
  check(set_attr_raw("path=agentpub;key=type;value=memory") == AGENT_TOOL_OK,
        "cache demo type attr");
  check(set_attr_raw("path=agentpub;key=owner;value=system") == AGENT_TOOL_OK,
        "cache demo owner attr");
  check(set_attr_raw("path=agentpub;key=tags;value=shared") == AGENT_TOOL_OK,
        "cache demo tags attr");

  check(call_tool("query_file", query, &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "cache_hit=0") &&
          contains(resp.result, "fs_scanned="),
        "first query_file populates shared cache");

  child = fork();
  if(child == 0){
    agent_create(AGENT_TYPE_WORKER, 0, 256);
    if(call_tool("query_file", query, &resp) == AGENT_TOOL_OK &&
       contains(resp.result, "cache_hit=1") &&
       contains(resp.result, "fs_scanned=0"))
      exit(0);
    printf("agentinnovationtest: child cache result %s\n", resp.result);
    exit(1);
  }
  check(wait(&status) == child && status == 0,
        "second agent reuses shared query cache");

  check(set_attr_raw("path=agentpub;key=owner;value=system") == AGENT_TOOL_OK,
        "cache invalidating attr write");
  check(call_tool("query_file", query, &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "cache_hit=0"),
        "metadata version invalidates stale cache");
}

static void
scheduler_worker(char kind, int ready_fd, int release_fd, int result_fd)
{
  struct agent_wait_event event;
  char ch;
  int reason;

  agent_create(AGENT_TYPE_WORKER, 0, 256);
  agent_priority_set(5);
  if(kind == 'M')
    agent_watch(AGENT_WATCH_MESSAGE);
  else if(kind == 'F')
    agent_watch(AGENT_WATCH_FILEMOD);
  else if(kind == 'H')
    agent_heartbeat_set(5);

  write(ready_fd, &kind, 1);
  read(release_fd, &ch, 1);

  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  if((kind == 'M' && (reason & AGENT_EVENT_MESSAGE)) ||
     (kind == 'F' && (reason & AGENT_EVENT_FILEMOD)) ||
     (kind == 'H' && (reason & AGENT_EVENT_HEARTBEAT))){
    if(kind == 'F')
      sleep(1);
    else if(kind == 'H')
      sleep(2);
    write(result_fd, &kind, 1);
    exit(0);
  }
  exit(1);
}

static void
event_scheduler_test(void)
{
  int ready[2];
  int releasep[2];
  int result[2];
  int mpid;
  int fpid;
  int hpid;
  int status = -1;
  char buf[4];

  pipe(ready);
  pipe(releasep);
  pipe(result);

  mpid = fork();
  if(mpid == 0)
    scheduler_worker('M', ready[1], releasep[0], result[1]);
  fpid = fork();
  if(fpid == 0)
    scheduler_worker('F', ready[1], releasep[0], result[1]);
  hpid = fork();
  if(hpid == 0)
    scheduler_worker('H', ready[1], releasep[0], result[1]);

  read_exact(ready[0], buf, 3);
  sleep(8);
  check(send_message_to(mpid, "urgent") == AGENT_TOOL_OK,
        "message event prepared");
  make_file("agentmod", "filemod event source");
  check(set_attr_raw("path=agentmod;key=owner;value=system") == AGENT_TOOL_OK,
        "filemod event prepared");

  write(releasep[1], "xxx", 3);
  memset(buf, 0, sizeof(buf));
  read_exact(result[0], buf, 3);
  check(buf[0] == 'M' && buf[1] == 'F' && buf[2] == 'H',
        "scheduler orders MESSAGE before FILEMOD before HEARTBEAT");

  check(wait(&status) > 0 && status == 0, "first scheduler worker exits");
  check(wait(&status) > 0 && status == 0, "second scheduler worker exits");
  check(wait(&status) > 0 && status == 0, "third scheduler worker exits");
}

static void
dynamic_tool_service(int ready_fd)
{
  struct agent_dynamic_tool_request req;
  char ok = 'R';

  agent_create(AGENT_TYPE_WORKER, 0, 256);
  if(tool_register("summarize_log", AGENT_TOOL_FLAG_PUBLIC) < 0)
    exit(1);
  write(ready_fd, &ok, 1);
  if(tool_recv(&req) < 0)
    exit(1);
  if(!contains(req.params, "file=agentlog"))
    exit(1);
  if(tool_reply(req.request_id,
                "{status=ok,tool=summarize_log,summary=agentlog compressed}",
                AGENT_TOOL_OK) < 0)
    exit(1);
  exit(0);
}

static void
dynamic_tool_test(void)
{
  struct agent_tool_response resp;
  int ready[2];
  int child;
  int status = -1;
  char ch;

  pipe(ready);
  child = fork();
  if(child == 0)
    dynamic_tool_service(ready[1]);
  read(ready[0], &ch, 1);

  check(call_tool("summarize_log", "file=agentlog", &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "summary=agentlog compressed"),
        "dynamic tool register/recv/reply");
  check(wait(&status) == child && status == 0,
        "dynamic tool service exits cleanly");
}

int
main(void)
{
  struct agent_info info;

  check((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 1024) > 0,
        "primary agent for innovation tests");
  check(agent_info(&info) == 0 && info.agent_priority == 5 &&
          info.agent_group == getpid(),
        "agent priority and group initialized");

  shared_cache_test();
  event_scheduler_test();
  dynamic_tool_test();

  if(failures){
    printf("agentinnovationtest: %d failures\n", failures);
    exit(1);
  }
  printf("agentinnovationtest: all tests passed\n");
  exit(0);
}
