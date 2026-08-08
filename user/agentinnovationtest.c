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
get_tool_list(char *buf, int size)
{
  int n;

  memset(buf, 0, size);
  n = tool_list(buf, size - 1);
  if(n >= 0 && n < size)
    buf[n] = 0;
  return n;
}

static uint64
last_context_sequence(struct agent_info *info)
{
  struct agent_context_header *hdr =
    (struct agent_context_header *)info->context_start;

  if(hdr->next_sequence <= 1)
    return 0;
  return hdr->next_sequence - 1;
}

static void
context_version_test(struct agent_info *info)
{
  struct agent_context_validation validation;
  struct agent_tool_response resp;
  uint64 no_dep_seq;
  uint64 read_seq;
  uint64 refreshed_seq;
  uint64 unrelated_seq;
  uint64 deleted_seq;
  uint64 rollback_seq;
  uint64 clear_seq;
  uint64 evicted_seq;
  struct agent_context_header *hdr =
    (struct agent_context_header *)info->context_start;
  int fd;

  check(call_tool("get_system_status", "", &resp) == AGENT_TOOL_OK,
        "context no-dependency tool call");
  no_dep_seq = last_context_sequence(info);
  memset(&validation, 0, sizeof(validation));
  check(context_validate(no_dep_seq, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_NO_DEP,
        "context_validate reports NO_DEP");

  make_file("ctxver", "version one");
  check(call_tool("read_file", "path=ctxver", &resp) == AGENT_TOOL_OK,
        "versioned read_file creates dependency");
  read_seq = last_context_sequence(info);
  memset(&validation, 0, sizeof(validation));
  check(context_validate(read_seq, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_VALID &&
          validation.recorded_version == validation.current_version,
        "context_validate reports VALID");

  fd = open("ctxver", O_RDWR);
  check(fd >= 0 && write(fd, "changed", 7) == 7,
        "modify versioned context file");
  if(fd >= 0)
    close(fd);
  memset(&validation, 0, sizeof(validation));
  check(context_validate(read_seq, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_STALE &&
          validation.current_version != validation.recorded_version,
        "context_validate reports STALE");

  check(call_tool("read_file", "path=ctxver", &resp) == AGENT_TOOL_OK,
        "read_file refreshes stale dependency");
  refreshed_seq = last_context_sequence(info);
  memset(&validation, 0, sizeof(validation));
  check(context_validate(refreshed_seq, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_VALID &&
          validation.recorded_version == validation.current_version,
        "re-read context dependency returns VALID");

  make_file("ctxother", "unrelated version");
  check(call_tool("read_file", "path=ctxver", &resp) == AGENT_TOOL_OK,
        "read dependency before unrelated modification");
  unrelated_seq = last_context_sequence(info);
  fd = open("ctxother", O_RDWR);
  check(fd >= 0 && write(fd, "other", 5) == 5,
        "modify unrelated context file");
  if(fd >= 0)
    close(fd);
  memset(&validation, 0, sizeof(validation));
  check(context_validate(unrelated_seq, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_VALID,
        "unrelated modification keeps context VALID");

  make_file("ctxgone", "delete me");
  check(call_tool("read_file", "path=ctxgone", &resp) == AGENT_TOOL_OK,
        "read_file dependency before unlink");
  deleted_seq = last_context_sequence(info);
  check(unlink("ctxgone") == 0, "unlink context dependency file");
  memset(&validation, 0, sizeof(validation));
  check(context_validate(deleted_seq, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_DELETED,
        "context_validate reports DELETED");

  memset(&validation, 0, sizeof(validation));
  check(context_validate(0xffff, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_NOT_FOUND,
        "context_validate reports NOT_FOUND");

  check(call_tool("read_file", "path=ctxver", &resp) == AGENT_TOOL_OK,
        "read dependency before rollback");
  rollback_seq = last_context_sequence(info);
  check(hdr->node_count > 1 && context_rollback(hdr->node_count - 1) == 0,
        "rollback removes dependency node");
  memset(&validation, 0, sizeof(validation));
  check(context_validate(rollback_seq, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_NOT_FOUND,
        "rolled-back dependency is NOT_FOUND");

  check(call_tool("read_file", "path=ctxver", &resp) == AGENT_TOOL_OK,
        "read dependency before clear");
  clear_seq = last_context_sequence(info);
  check(context_clear() == 0, "clear removes dependency nodes");
  memset(&validation, 0, sizeof(validation));
  check(context_validate(clear_seq, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_NOT_FOUND,
        "cleared dependency is NOT_FOUND");

  check(call_tool("read_file", "path=ctxver", &resp) == AGENT_TOOL_OK,
        "read dependency before eviction");
  evicted_seq = last_context_sequence(info);
  for(int i = 0; i < AGENT_CONTEXT_MAX_NODES + 4; i++)
    call_tool("get_system_status", "", &resp);
  memset(&validation, 0, sizeof(validation));
  check(context_validate(evicted_seq, &validation) == 0 &&
          validation.state == AGENT_CONTEXT_NOT_FOUND,
        "evicted dependency is NOT_FOUND");
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
shared_cache_invalidation_test(void)
{
  struct agent_tool_response resp;
  const char *old_query =
    "type=code;module=todo;tags=delete;public=true;keyword=delete";
  const char *new_query =
    "type=code;module=todo;tags=updated;public=true;keyword=delete";

  make_file("cachetodo", "delete task cache invalidation sample");
  check(set_attr_raw("path=cachetodo;key=type;value=code") == AGENT_TOOL_OK,
        "cache invalidation type attr");
  check(set_attr_raw("path=cachetodo;key=module;value=todo") == AGENT_TOOL_OK,
        "cache invalidation module attr");
  check(set_attr_raw("path=cachetodo;key=tags;value=delete") == AGENT_TOOL_OK,
        "cache invalidation tags attr");

  check(call_tool("query_file", old_query, &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "cache_hit=0") &&
          contains(resp.result, "cachetodo"),
        "old query populates shared cache");
  check(call_tool("query_file", old_query, &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "cache_hit=1"),
        "old query hits shared cache before attr change");

  check(set_attr_raw("path=cachetodo;key=tags;value=updated") == AGENT_TOOL_OK,
        "cache invalidation updates tags attr");
  check(call_tool("query_file", old_query, &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "cache_hit=0") &&
          !contains(resp.result, "cachetodo"),
        "old shared cache entry invalidated after attr change");
  check(call_tool("query_file", new_query, &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "cachetodo"),
        "updated attr query returns fresh result");
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
  if((kind == 'M' && reason == AGENT_WAIT_MESSAGE) ||
     (kind == 'F' && reason == AGENT_WAIT_FILEMOD) ||
     (kind == 'H' && reason == AGENT_WAIT_HEARTBEAT)){
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
basic_dynamic_tool_service(int ready_fd)
{
  struct agent_dynamic_tool_request req;
  char ok = 'R';

  agent_create(AGENT_TYPE_WORKER, 0, 256);
  if(agent_role_set(AGENT_ROLE_TOOL_SERVICE) < 0)
    exit(1);
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
dynamic_tool_basic_test(void)
{
  struct agent_tool_response resp;
  int ready[2];
  int child;
  int status = -1;
  char ch;

  pipe(ready);
  child = fork();
  if(child == 0)
    basic_dynamic_tool_service(ready[1]);
  read(ready[0], &ch, 1);

  check(call_tool("summarize_log", "file=agentlog", &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "summary=agentlog compressed"),
        "dynamic tool register/recv/reply");
  check(wait(&status) == child && status == 0,
        "dynamic tool service exits cleanly");
  check(call_tool("summarize_log", "file=agentlog", &resp) ==
          AGENT_TOOL_ERR_TOOL_NOT_FOUND,
        "dynamic tool call fails after service exit cleanup");
}

static void
schema_dynamic_tool_service(int ready_fd, int release_fd, const char *name,
                            int flags)
{
  char ok = 'R';
  char ch;

  agent_create(AGENT_TYPE_WORKER, 0, 256);
  if(agent_role_set(AGENT_ROLE_TOOL_SERVICE) < 0)
    exit(1);
  if(tool_register(name, flags) < 0)
    exit(1);
  write(ready_fd, &ok, 1);
  read(release_fd, &ch, 1);
  exit(0);
}

static void
dynamic_tool_schema_lifecycle_test(void)
{
  struct agent_tool_schema schema;
  char tools[512];
  int ready[2];
  int releasep[2];
  int child;
  int status = -1;
  char ch;

  pipe(ready);
  pipe(releasep);
  child = fork();
  if(child == 0)
    schema_dynamic_tool_service(ready[1], releasep[0], "schema_tool",
                                AGENT_TOOL_FLAG_PUBLIC);
  read(ready[0], &ch, 1);

  check(get_tool_list(tools, sizeof(tools)) > 0 &&
          contains(tools, "schema_tool(dynamic)"),
        "tool_list reports dynamic tool");
  memset(&schema, 0, sizeof(schema));
  check(tool_schema("schema_tool", &schema) == 0 &&
          schema.is_dynamic == 1 &&
          schema.owner_pid == child &&
          schema.flags == AGENT_TOOL_FLAG_PUBLIC &&
          strcmp(schema.name, "schema_tool") == 0,
        "tool_schema reports dynamic owner and flags");

  write(releasep[1], "x", 1);
  check(wait(&status) == child && status == 0,
        "schema tool service exits cleanly");
  check(get_tool_list(tools, sizeof(tools)) > 0 &&
          !contains(tools, "schema_tool(dynamic)"),
        "dynamic tool removed from list after service exit");
  memset(&schema, 0, sizeof(schema));
  check(tool_schema("schema_tool", &schema) < 0,
        "tool_schema fails after dynamic tool unregister");
}

static void
register_only_service(int result_fd, const char *name, int flags)
{
  int ret;

  agent_create(AGENT_TYPE_WORKER, 0, 256);
  ret = agent_role_set(AGENT_ROLE_TOOL_SERVICE);
  if(ret == AGENT_TOOL_OK)
    ret = tool_register(name, flags);
  write(result_fd, &ret, sizeof(ret));
  sleep(20);
  exit(0);
}

static void
dynamic_tool_register_reject_test(void)
{
  char tools[512];
  int result[2];
  int child1;
  int child2;
  int worker;
  int ret1 = 0;
  int ret2 = 0;
  int ret3 = 0;
  int status = -1;

  pipe(result);
  child1 = fork();
  if(child1 == 0)
    register_only_service(result[1], "dup_tool", AGENT_TOOL_FLAG_PUBLIC);
  read(result[0], &ret1, sizeof(ret1));
  check(ret1 == AGENT_TOOL_OK, "first dynamic duplicate-name register succeeds");

  child2 = fork();
  if(child2 == 0)
    register_only_service(result[1], "dup_tool", AGENT_TOOL_FLAG_PUBLIC);
  read(result[0], &ret2, sizeof(ret2));
  check(ret2 == AGENT_TOOL_ERR_BUSY,
        "duplicate dynamic tool register returns BUSY");

  worker = fork();
  if(worker == 0){
    agent_create(AGENT_TYPE_WORKER, 0, 256);
    ret3 = tool_register("bad_dyn_tool", AGENT_TOOL_FLAG_PUBLIC);
    write(result[1], &ret3, sizeof(ret3));
    exit(0);
  }
  read(result[0], &ret3, sizeof(ret3));
  check(wait(&status) == worker && status == 0,
        "unauthorized register worker exits cleanly");
  check(ret3 == AGENT_TOOL_ERR_PERMISSION,
        "worker without register capability cannot register tool");
  check(get_tool_list(tools, sizeof(tools)) > 0 &&
          !contains(tools, "bad_dyn_tool(dynamic)"),
        "rejected dynamic tool register leaves table unchanged");

  kill(child1);
  kill(child2);
  check(wait(&status) > 0, "first duplicate service reaped");
  check(wait(&status) > 0, "second duplicate service reaped");
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

  context_version_test(&info);
  shared_cache_test();
  shared_cache_invalidation_test();
  event_scheduler_test();
  dynamic_tool_basic_test();
  dynamic_tool_schema_lifecycle_test();
  dynamic_tool_register_reject_test();

  if(failures){
    printf("agentinnovationtest: %d failures\n", failures);
    exit(1);
  }
  printf("agentinnovationtest: all tests passed\n");
  exit(0);
}
