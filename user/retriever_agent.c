#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"

static struct agent_wait_event g_event;
static struct agent_tool_response g_resp;

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
result_uint(const char *result, const char *key, int fallback)
{
  int keylen = strlen(key);

  for(const char *p = result; *p; p++){
    if(memcmp(p, key, keylen) == 0 && p[keylen] == '='){
      int value = 0;
      int digits = 0;

      p += keylen + 1;
      while(*p >= '0' && *p <= '9'){
        value = value * 10 + *p++ - '0';
        digits++;
      }
      return digits ? value : fallback;
    }
  }
  return fallback;
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
  char path[64];
  char metric_message[AGENT_MESSAGE_MAX];
  int patch_pid;
  int planner_pid;
  int reviewer_pid;
  int pos;

  if((uint64)agent_create(AGENT_TYPE_WORKER, 0, 1024) == 0)
    exit(1);
  if(agent_role_set(AGENT_ROLE_RETRIEVER) < 0)
    exit(1);
  if(agent_sched_set(8, 8) < 0)
    exit(1);
  agent_watch(AGENT_WATCH_MESSAGE);
  push_context_note("retriever boot", "waiting for planner assignment");
  memset(&g_event, 0, sizeof(g_event));
  if(agent_wait(1, &g_event) < 0 || g_event.reason != AGENT_WAIT_MESSAGE)
    exit(1);
  push_context_note("planner message", "received retriever assignment");

  if(parse_uint_param(g_event.message, "patch_pid", &patch_pid) < 0 ||
     parse_uint_param(g_event.message, "reviewer_pid", &reviewer_pid) < 0 ||
     parse_uint_param(g_event.message, "planner_pid", &planner_pid) < 0 ||
     param_value(g_event.message, "path", path, sizeof(path)) < 0){
    printf("retriever_agent: bad planner message\n");
    exit(1);
  }

  if(call_tool("query_file", "type=code;module=todo;keyword=delete", &g_resp) != AGENT_TOOL_OK){
    printf("retriever_agent: query_file failed\n");
    exit(1);
  }
  push_context_note("query_file(type=code,module=todo,keyword=delete)",
                    g_resp.result);
  memset(metric_message, 0, sizeof(metric_message));
  pos = 0;
  append_str(metric_message, &pos,
             "stage=retriever;status=query_cache;cache_hit=",
             sizeof(metric_message));
  append_uint(metric_message, &pos,
              result_uint(g_resp.result, "cache_hit", 0),
              sizeof(metric_message));
  append_str(metric_message, &pos, ";used_index=", sizeof(metric_message));
  append_uint(metric_message, &pos,
              result_uint(g_resp.result, "used_index", 0),
              sizeof(metric_message));
  append_str(metric_message, &pos, ";scanned=", sizeof(metric_message));
  append_uint(metric_message, &pos,
              result_uint(g_resp.result, "cache_hit", 0) ?
              result_uint(g_resp.result, "fs_scanned", 0) :
              result_uint(g_resp.result, "index_scanned", 0),
              sizeof(metric_message));
  append_str(metric_message, &pos, ";matches=", sizeof(metric_message));
  append_uint(metric_message, &pos,
              result_uint(g_resp.result, "count", 0),
              sizeof(metric_message));
  send_message_to(planner_pid, metric_message);
  sleep(5);
  send_message_to(reviewer_pid, "stage=retriever;status=cache_probe");
  sleep(10);

  if(call_tool("read_file", "path=repo/todo.c", &g_resp) != AGENT_TOOL_OK){
    printf("retriever_agent: read_file failed\n");
    send_message_to(planner_pid, "stage=retriever;status=read_failed");
    exit(1);
  }
  if(!contains(g_resp.result, "delete_task"))
    send_message_to(planner_pid, "stage=retriever;status=read_truncated");
  push_context_note("read_file(path=repo/todo.c)",
                    "delete_task misses task_count--");

  if(send_message_to(patch_pid,
                     "path=repo/todo.c;op=replace;old=// BUG: missing task_count--;new=task_count--%3B") < 0){
    exit(1);
  }
  send_message_to(planner_pid, "stage=retriever;status=found_bug;file=repo/todo.c");
  push_context_note("send_message(patch)", "forwarded patch request");
  exit(0);
}
