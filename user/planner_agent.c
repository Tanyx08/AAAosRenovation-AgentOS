#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

static struct agent_info g_info;
static struct agent_tool_response g_resp;
static struct agent_wait_event g_event;
static char g_retriever_msg[AGENT_MESSAGE_MAX];
static char g_patch_msg[AGENT_MESSAGE_MAX];
static char g_test_msg[AGENT_MESSAGE_MAX];
static char g_reviewer_msg[AGENT_MESSAGE_MAX];
static char g_plan_summary[128];
static char g_file_summary[96];
static char g_patch_summary[96];
static char g_test_summary[160];
static char g_review_summary[96];
static char g_context_dump[768];

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
call_tool(const char *tool, const char *params, struct agent_tool_response *resp)
{
  struct agent_tool_request req;

  memset(&req, 0, sizeof(req));
  strcpy(req.tool, tool);
  strcpy(req.params, params);
  return tool_call(&req, resp);
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

static __attribute__((noinline)) void
read_repo_bug(void)
{
  int fd;
  char buf[768];
  int n;

  fd = open("/repo/todo.c", O_RDONLY);
  if(fd < 0)
    fd = open("repo/todo.c", O_RDONLY);
  if(fd < 0){
    printf("planner_agent: cannot open /repo/todo.c\n");
    return;
  }
  memset(buf, 0, sizeof(buf));
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);

  if(n > 0 && contains(buf, "missing task_count--")){
    printf("planner_agent: read repo bug marker from todo.c\n");
    push_context_note("read /repo/todo.c", "found missing task_count-- bug marker");
  } else {
    printf("planner_agent: repo file read ok, bug marker not in first chunk\n");
    push_context_note("read /repo/todo.c", "repo file readable");
  }
}

static __attribute__((noinline)) int
spawn_agent(const char *prog)
{
  int pid;
  char *argv[2];

  pid = fork();
  if(pid != 0)
    return pid;

  argv[0] = (char*)prog;
  argv[1] = 0;
  exec(prog, argv);
  printf("planner_agent: exec %s failed\n", prog);
  exit(1);
}

int
main(int argc, char **argv)
{
  char *task;
  int retriever_pid;
  int patch_pid;
  int test_pid;
  int reviewer_pid;
  int status = 0;
  int got_review = 0;
  int pos;

  task = "fix todo delete bug";
  if(argc > 1)
    task = argv[1];

  if((uint64)agent_create(AGENT_TYPE_PRIMARY, 5, 1024) == 0){
    printf("planner_agent: agent_create failed\n");
    exit(1);
  }
  agent_watch(AGENT_WATCH_MESSAGE);

  printf("planner_agent: task=%s\n", task);
  copy_limited(g_plan_summary, "find files -> inspect bug -> patch -> test -> review",
               sizeof(g_plan_summary));
  copy_limited(g_file_summary, "repo/todo.c located", sizeof(g_file_summary));
  copy_limited(g_patch_summary, "patch pending", sizeof(g_patch_summary));
  copy_limited(g_test_summary, "test pending", sizeof(g_test_summary));
  copy_limited(g_review_summary, "review pending", sizeof(g_review_summary));
  push_context_note("task intake", task);

  memset(&g_event, 0, sizeof(g_event));
  printf("planner_agent: waiting for first heartbeat\n");
  if(agent_wait(1, &g_event) < 0 || (g_event.reason & AGENT_EVENT_HEARTBEAT) == 0){
    printf("planner_agent: heartbeat wait failed\n");
    exit(1);
  }

  printf("planner_agent: heartbeat wakeup, building plan\n");

  read_repo_bug();

  retriever_pid = spawn_agent("retriever_agen");
  patch_pid = spawn_agent("patch_agent");
  test_pid = spawn_agent("test_agent");
  reviewer_pid = spawn_agent("reviewer_agent");

  if(retriever_pid < 0 || patch_pid < 0 || test_pid < 0 || reviewer_pid < 0){
    printf("planner_agent: failed to create worker agents\n");
    exit(1);
  }

  printf("planner_agent: spawned retriever=%d patch=%d test=%d reviewer=%d\n",
         retriever_pid, patch_pid, test_pid, reviewer_pid);
  push_context_note("spawn workers",
                    "retriever patch test reviewer created");

  sleep(10);

  memset(g_retriever_msg, 0, sizeof(g_retriever_msg));
  pos = 0;
  append_str(g_retriever_msg, &pos,
             "role=retriever;path=repo/todo.c;patch_pid=",
             sizeof(g_retriever_msg));
  append_uint(g_retriever_msg, &pos, patch_pid, sizeof(g_retriever_msg));
  append_str(g_retriever_msg, &pos, ";planner_pid=", sizeof(g_retriever_msg));
  append_uint(g_retriever_msg, &pos, getpid(), sizeof(g_retriever_msg));
  memset(g_patch_msg, 0, sizeof(g_patch_msg));
  pos = 0;
  append_str(g_patch_msg, &pos,
             "role=patch;path=repo/todo.c;test_pid=",
             sizeof(g_patch_msg));
  append_uint(g_patch_msg, &pos, test_pid, sizeof(g_patch_msg));
  append_str(g_patch_msg, &pos, ";planner_pid=", sizeof(g_patch_msg));
  append_uint(g_patch_msg, &pos, getpid(), sizeof(g_patch_msg));
  memset(g_test_msg, 0, sizeof(g_test_msg));
  pos = 0;
  append_str(g_test_msg, &pos, "role=test;reviewer_pid=", sizeof(g_test_msg));
  append_uint(g_test_msg, &pos, reviewer_pid, sizeof(g_test_msg));
  append_str(g_test_msg, &pos, ";planner_pid=", sizeof(g_test_msg));
  append_uint(g_test_msg, &pos, getpid(), sizeof(g_test_msg));
  memset(g_reviewer_msg, 0, sizeof(g_reviewer_msg));
  pos = 0;
  append_str(g_reviewer_msg, &pos, "role=reviewer;watch=repo/todo.c;planner_pid=",
             sizeof(g_reviewer_msg));
  append_uint(g_reviewer_msg, &pos, getpid(), sizeof(g_reviewer_msg));
  send_message_to(retriever_pid, g_retriever_msg);
  send_message_to(patch_pid, g_patch_msg);
  send_message_to(test_pid, g_test_msg);
  send_message_to(reviewer_pid, g_reviewer_msg);
  printf("planner_agent: dispatched worker assignments\n");
  push_context_note("dispatch workers", "initial role messages sent");

  while(!got_review){
    char stage[24];
    char status_value[64];

    memset(&g_event, 0, sizeof(g_event));
    if(agent_wait(1, &g_event) < 0)
      break;
    if((g_event.reason & AGENT_EVENT_MESSAGE) == 0)
      continue;
    printf("planner_agent: worker update: %s\n", g_event.message);
    push_context_note("worker message", g_event.message);
    if(param_value(g_event.message, "stage", stage, sizeof(stage)) == 0 &&
       param_value(g_event.message, "status", status_value,
                   sizeof(status_value)) == 0){
      if(contains(stage, "retriever"))
        copy_limited(g_file_summary, "repo/todo.c found and bug localized",
                     sizeof(g_file_summary));
      else if(contains(stage, "patch"))
        copy_limited(g_patch_summary, "repo/todo.c patched with task_count--;",
                     sizeof(g_patch_summary));
    } else if(contains(g_event.message, "target=todo_delete")){
      copy_limited(g_test_summary, g_event.message, sizeof(g_test_summary));
    } else if(contains(g_event.message, "review=")){
      copy_limited(g_review_summary, g_event.message, sizeof(g_review_summary));
      got_review = 1;
    }
  }

  call_tool("diff_file", "path=repo/todo.c", &g_resp);
  context_query(g_context_dump, sizeof(g_context_dump) - 1);

  while(wait(&status) > 0)
    ;

  agent_wait(0, 0);
  agent_info(&g_info);

  printf("planner_agent: summary\n");
  printf("  plan: %s\n", g_plan_summary);
  printf("  key file: %s\n", g_file_summary);
  printf("  patch: %s\n", g_patch_summary);
  printf("  test: %s\n", g_test_summary);
  printf("  review: %s\n", g_review_summary);
  printf("  diff: %s\n", g_resp.result);
  printf("  planner context: %s\n", g_context_dump);
  printf("  final loop_state=%d\n", g_info.loop_state);
  exit(0);
}
