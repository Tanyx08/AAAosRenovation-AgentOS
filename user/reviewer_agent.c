#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"

static struct agent_wait_event g_event;
static struct agent_tool_response g_resp;
static char g_path[64];
static char g_diff_params[AGENT_TOOL_PARAM_MAX];

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
  int planner_pid;
  int got_filemod = 0;
  int got_test = 0;
  int test_pass = 0;

  if((uint64)agent_create(AGENT_TYPE_WORKER, 0, 1024) == 0)
    exit(1);
  agent_watch(AGENT_WATCH_MESSAGE);
  push_context_note("reviewer boot", "waiting for planner assignment");

  memset(&g_event, 0, sizeof(g_event));
  if(agent_wait(1, &g_event) < 0 || (g_event.reason & AGENT_EVENT_MESSAGE) == 0)
    exit(1);
  printf("reviewer_agent: setup=%s\n", g_event.message);
  push_context_note("planner message", "received reviewer setup");

  if(param_value(g_event.message, "watch", g_path, sizeof(g_path)) < 0 ||
     parse_uint_param(g_event.message, "planner_pid", &planner_pid) < 0)
    exit(1);
  if(agent_watch_file(g_path) < 0)
    exit(1);
  push_context_note("agent_watch_file", g_path);

  while(!got_filemod || !got_test){
    memset(&g_event, 0, sizeof(g_event));
    if(agent_wait(1, &g_event) < 0)
      exit(1);
    if(g_event.reason & AGENT_EVENT_FILEMOD){
      got_filemod = 1;
      printf("reviewer_agent: filemod=%s\n", g_event.file);
      push_context_note("filemod event", g_event.file);
    }
    if(g_event.reason & AGENT_EVENT_MESSAGE){
      got_test = 1;
      printf("reviewer_agent: test result=%s\n", g_event.message);
      push_context_note("test result message", "received rule test result");
      if(contains(g_event.message, "passed=3"))
        test_pass = 1;
    }
  }

  memset(g_diff_params, 0, sizeof(g_diff_params));
  strcpy(g_diff_params, "path=");
  copy_limited(g_diff_params + 5, g_path, sizeof(g_diff_params) - 5);
  if(call_tool("diff_file", g_diff_params, &g_resp) < 0)
    exit(1);
  printf("reviewer_agent: diff=%s\n", g_resp.result);
  push_context_note("diff_file", g_resp.result);

  if(test_pass && contains(g_resp.result, "task_count--;")){
    send_message_to(planner_pid, "review=approve;status=done");
    push_context_note("review decision", "approved patch");
  } else {
    send_message_to(planner_pid, "review=reject;status=needs_fix");
    push_context_note("review decision", "rejected patch");
  }
  exit(0);
}
