#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

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
copy_limited(char *dst, const char *src, int max)
{
  int i = 0;

  if(max <= 0)
    return 0;
  while(src[i] && i < max - 1){
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
  return i;
}

static int
param_value(const char *params, const char *key, char *out, int outsz)
{
  int keylen = strlen(key);
  const char *p = params;

  while(*p){
    if(memcmp(p, key, keylen) == 0 && p[keylen] == '='){
      int n = 0;

      p += keylen + 1;
      while(*p && *p != ';' && n < outsz - 1)
        out[n++] = *p++;
      out[n] = 0;
      return 0;
    }
    while(*p && *p != ';')
      p++;
    if(*p == ';')
      p++;
  }
  if(outsz > 0)
    out[0] = 0;
  return -1;
}

static int
parse_uint_param(const char *params, const char *key, int *value)
{
  char tmp[24];
  int ret = 0;

  if(param_value(params, key, tmp, sizeof(tmp)) < 0 || tmp[0] == 0)
    return -1;
  for(int i = 0; tmp[i]; i++){
    if(tmp[i] < '0' || tmp[i] > '9')
      return -1;
    ret = ret * 10 + tmp[i] - '0';
  }
  *value = ret;
  return 0;
}

static int
read_file(const char *path, char *buf, int bufsz)
{
  int fd;
  int n;

  fd = open(path, O_RDONLY);
  if(fd < 0)
    return -1;
  memset(buf, 0, bufsz);
  n = read(fd, buf, bufsz - 1);
  close(fd);
  if(n < 0)
    return -1;
  buf[n] = 0;
  return n;
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
  static struct agent_tool_response resp;
  static char params[AGENT_TOOL_PARAM_MAX];
  int pos = 0;

  memset(params, 0, sizeof(params));
  append_str(params, &pos, "target_pid=", sizeof(params));
  append_uint(params, &pos, pid, sizeof(params));
  append_str(params, &pos, ";message=", sizeof(params));
  append_str(params, &pos, message, sizeof(params));
  return call_tool("send_message", params, &resp);
}

static void
push_context_note(const char *request, const char *result)
{
  struct agent_context_node node;

  memset(&node, 0, sizeof(node));
  node.timestamp_ms = uptime();
  copy_limited(node.request, request, sizeof(node.request));
  copy_limited(node.result, result, sizeof(node.result));
  context_push(&node);
}

static void
run_rule_test_dyn(struct agent_dynamic_tool_request *req)
{
  static char todo[768];
  static char testsrc[512];
  static char result[AGENT_TOOL_RESULT_MAX];
  int pass_count = 0;
  int total = 3;
  int has_count;
  int has_delete;
  int has_test;

  if(!contains(req->params, "target=todo_delete")){
    tool_reply(req->request_id,
               "{status=error,tool=run_rule_test_dyn,reason=unknown_target}",
               AGENT_TOOL_ERR_BAD_PARAM);
    return;
  }

  if(read_file("repo/todo.c", todo, sizeof(todo)) < 0 ||
     read_file("repo/test.c", testsrc, sizeof(testsrc)) < 0){
    tool_reply(req->request_id,
               "{status=error,tool=run_rule_test_dyn,reason=files_unavailable}",
               AGENT_TOOL_ERR_BAD_PARAM);
    return;
  }

  has_count = contains(todo, "task_count--");
  has_delete = contains(todo, "delete_task");
  has_test = contains(testsrc, "delete_task") ||
             contains(testsrc, "test_delete_updates_count");
  if(has_count)
    pass_count++;
  if(has_delete)
    pass_count++;
  if(has_test)
    pass_count++;

  memset(result, 0, sizeof(result));
  {
    int pos = 0;
    append_str(result, &pos, "{status=", sizeof(result));
    append_str(result, &pos, pass_count == total ? "ok" : "fail",
               sizeof(result));
    append_str(result, &pos,
               ",tool=run_rule_test_dyn,target=todo_delete,checks=", 
               sizeof(result));
    append_str(result, &pos, has_count ? "C" : "c", sizeof(result));
    append_str(result, &pos, has_delete ? "D" : "d", sizeof(result));
    append_str(result, &pos, has_test ? "T" : "t", sizeof(result));
    append_str(result, &pos, ",passed=", sizeof(result));
    append_uint(result, &pos, pass_count, sizeof(result));
    append_str(result, &pos, ",total=", sizeof(result));
    append_uint(result, &pos, total, sizeof(result));
    append_str(result, &pos, "}", sizeof(result));
  }

  push_context_note("run_rule_test_dyn", result);
  tool_reply(req->request_id, result,
             pass_count == total ? AGENT_TOOL_OK : AGENT_TOOL_ERR_BAD_PARAM);
}

int
main(int argc, char **argv)
{
  struct agent_dynamic_tool_request req;
  int planner_pid = 0;

  if(argc > 1)
    parse_uint_param(argv[1], "planner_pid", &planner_pid);

  if((uint64)agent_create(AGENT_TYPE_WORKER, 0, 1024) == 0)
    exit(1);
  if(agent_role_set(AGENT_ROLE_TOOL_SERVICE) < 0)
    exit(1);
  if(agent_sched_set(4, 3) < 0)
    exit(1);
  if(tool_register("run_rule_test_dyn", AGENT_TOOL_FLAG_PUBLIC) < 0)
    exit(1);
  push_context_note("tool_register", "run_rule_test_dyn registered");
  if(planner_pid > 0)
    send_message_to(planner_pid,
                    "stage=tool;status=ready;tool=run_rule_test_dyn;sched=4/3");

  memset(&req, 0, sizeof(req));
  if(tool_recv(&req) < 0)
    exit(1);
  if(contains(req.tool, "run_rule_test_dyn"))
    run_rule_test_dyn(&req);
  else
    tool_reply(req.request_id,
               "{status=error,tool=run_rule_test_dyn,reason=bad_tool}",
               AGENT_TOOL_ERR_TOOL_NOT_FOUND);
  exit(0);
}
