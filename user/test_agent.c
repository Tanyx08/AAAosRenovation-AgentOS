#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"

static struct agent_wait_event g_event;
static struct agent_tool_response g_resp;

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

static void
copy_limited(char *dst, const char *src, int max)
{
  int i = 0;

  if(max <= 0)
    return;
  while(src[i] && i < max - 1){
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
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

int
main(void)
{
  int reviewer_pid;
  int planner_pid;

  if((uint64)agent_create(AGENT_TYPE_WORKER, 0, 1024) == 0)
    exit(1);
  if(agent_role_set(AGENT_ROLE_TEST) < 0)
    exit(1);
  if(agent_sched_set(5, 4) < 0)
    exit(1);
  agent_watch(AGENT_WATCH_MESSAGE);
  push_context_note("test boot", "waiting for planner assignment");

  memset(&g_event, 0, sizeof(g_event));
  if(agent_wait(1, &g_event) < 0 || g_event.reason != AGENT_WAIT_MESSAGE)
    exit(1);
  push_context_note("planner message", "received test setup");
  if(parse_uint_param(g_event.message, "reviewer_pid", &reviewer_pid) < 0 ||
     parse_uint_param(g_event.message, "planner_pid", &planner_pid) < 0)
    exit(1);
  send_message_to(planner_pid, "stage=test;status=ready;sched=5/4");

  memset(&g_event, 0, sizeof(g_event));
  if(agent_wait(1, &g_event) < 0 || g_event.reason != AGENT_WAIT_MESSAGE)
    exit(1);
  push_context_note("patch message", "received patch completion");

  call_tool("run_rule_test_dyn", "target=todo_delete", &g_resp);
  push_context_note("run_rule_test_dyn", g_resp.result);
  if(send_message_to(planner_pid, g_resp.result) < 0)
    exit(1);
  sleep(5);
  if(send_message_to(reviewer_pid, g_resp.result) < 0)
    exit(1);
  exit(0);
}
