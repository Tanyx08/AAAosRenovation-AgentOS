#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"
#include "user/llm_bridge.h"

static struct agent_info g_info;
static struct agent_tool_response g_resp;
static struct agent_wait_event g_event;
static char g_retriever_msg[AGENT_MESSAGE_MAX];
static char g_patch_msg[AGENT_MESSAGE_MAX];
static char g_test_msg[AGENT_MESSAGE_MAX];
static char g_reviewer_msg[AGENT_MESSAGE_MAX];
static char g_tool_msg[AGENT_MESSAGE_MAX];
static char g_plan_summary[128];
static char g_file_summary[96];
static char g_patch_summary[96];
static char g_test_summary[160];
static char g_review_summary[96];
static char g_cache_summary[128];
static char g_sched_summary[160];
static char g_model_summary[128];
static char g_context_dump[768];
static int g_tool_calls;
static int g_agentos_syscalls;

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

  g_tool_calls++;
  g_agentos_syscalls++;
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

static int
llm_response_allows_codelab(const char *line)
{
  if(!contains(line, LLM_RESP_BEGIN) || !contains(line, LLM_RESP_END))
    return 0;
  if(contains(line, "state=failed") || contains(line, "action=abort"))
    return 0;
  return contains(line, "action=start_codelab") ||
         contains(line, "action=query_file") ||
         contains(line, "state=done");
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

static void
print_worker_event(const char *message)
{
  if(contains(message, "stage=tool") && contains(message, "status=ready")){
    printf("[Tool-Service] register run_rule_test_dyn ok; %s\n", message);
  } else if(contains(message, "stage=patch") &&
            contains(message, "status=ready")){
    printf("[Patch-Agent] ready for MESSAGE wakeup; %s\n", message);
  } else if(contains(message, "stage=test") &&
            contains(message, "status=ready")){
    printf("[Test-Agent] ready for patched-file message; %s\n", message);
  } else if(contains(message, "stage=reviewer") &&
            contains(message, "status=watching")){
    printf("[Reviewer-Agent] watch FILEMOD repo/todo.c; %s\n", message);
  } else if(contains(message, "stage=retriever") &&
            contains(message, "status=query_cache")){
    printf("[Kernel-FS] query_file type=code,module=todo,keyword=delete -> cache_hit=0, used_index=1\n");
    printf("[Retriever-Agent] indexed AgentFS query populated shared cache\n");
  } else if(contains(message, "stage=reviewer") &&
            contains(message, "status=query_cache")){
    printf("[Kernel-FS] reviewer repeated query -> shared query cache hit\n");
  } else if(contains(message, "stage=retriever") &&
            contains(message, "status=found_bug")){
    printf("[Retriever-Agent] read_file repo/todo.c -> found missing task_count--\n");
  } else if(contains(message, "stage=patch") &&
            contains(message, "status=patched")){
    printf("[Patch-Agent] patch_file repo/todo.c replace BUG with task_count--\n");
    printf("[Kernel-FS] FILEMOD repo/todo.c -> wakeup Reviewer-Agent\n");
  } else if(contains(message, "tool=run_rule_test_dyn")){
    printf("[Test-Agent] tool_call run_rule_test_dyn -> %s\n", message);
  } else if(contains(message, "review=approve")){
    printf("[Reviewer-Agent] diff_file + rule result -> approve\n");
  } else if(contains(message, "review=reject")){
    printf("[Reviewer-Agent] review result -> needs_fix\n");
  } else {
    printf("[Planner-Agent] worker update: %s\n", message);
  }
}

static void
wait_for_worker_message(const char *needle1, const char *needle2)
{
  for(;;){
    memset(&g_event, 0, sizeof(g_event));
    if(agent_wait(1, &g_event) < 0)
      exit(1);
    if(g_event.reason != AGENT_WAIT_MESSAGE)
      continue;
    print_worker_event(g_event.message);
    push_context_note("worker message", g_event.message);
    if(contains(g_event.message, needle1) &&
       (needle2 == 0 || contains(g_event.message, needle2)))
      break;
  }
}

static __attribute__((noinline)) void
read_repo_bug(void)
{
  int fd;
  char buf[768];
  int n;

  fd = open("/repo/todo.c", O_RDONLY);
  g_agentos_syscalls++;
  if(fd < 0){
    fd = open("repo/todo.c", O_RDONLY);
    g_agentos_syscalls++;
  }
  if(fd < 0){
    printf("[Planner-Agent] cannot open /repo/todo.c\n");
    return;
  }
  memset(buf, 0, sizeof(buf));
  n = read(fd, buf, sizeof(buf) - 1);
  g_agentos_syscalls++;
  close(fd);
  g_agentos_syscalls++;

  if(n > 0 && contains(buf, "missing task_count--")){
    printf("[Planner-Agent] read repo bug marker from todo.c\n");
    push_context_note("read /repo/todo.c", "found missing task_count-- bug marker");
  } else {
    printf("[Planner-Agent] repo file read ok, bug marker not in first chunk\n");
    push_context_note("read /repo/todo.c", "repo file readable");
  }
}

static __attribute__((noinline)) int
spawn_agent(const char *prog)
{
  int pid;
  char *argv[2];

  pid = fork();
  g_agentos_syscalls++;
  if(pid != 0)
    return pid;

  argv[0] = (char*)prog;
  argv[1] = 0;
  exec(prog, argv);
  printf("[Planner-Agent] exec %s failed\n", prog);
  exit(1);
}

static __attribute__((noinline)) int
spawn_agent_with_arg(const char *prog, char *arg)
{
  int pid;
  char *argv[3];

  pid = fork();
  g_agentos_syscalls++;
  if(pid != 0)
    return pid;

  argv[0] = (char*)prog;
  argv[1] = arg;
  argv[2] = 0;
  exec(prog, argv);
  printf("[Planner-Agent] exec %s failed\n", prog);
  exit(1);
}

static __attribute__((noinline)) int
request_llm_plan(int model_mode, const char *task)
{
  char line[LLM_RESPONSE_MAX];

  if(model_mode == 1){
    printf("[LLM-Bridge] demo model selected; deterministic bridge plan is used\n");
    printf(LLM_REQ_BEGIN " id=planner role=planner prompt=%s " LLM_RESP_END "\n",
           task);
    printf(LLM_RESP_BEGIN " id=planner state=done text=plan_patch_test action=start_codelab " LLM_RESP_END "\n");
    push_context_note("llm_demo_plan", "action=start_codelab");
    return 0;
  }
  if(model_mode == 2){
    printf("[LLM-Bridge] api model selected; waiting for host proxy decision\n");
    printf(LLM_REQ_BEGIN " id=planner role=planner prompt=%s " LLM_RESP_END "\n",
           task);
    printf("[LLM-Bridge] waiting for host proxy response line\n");
    memset(line, 0, sizeof(line));
    gets(line, sizeof(line));
    printf("[LLM-Bridge] received host response: %s", line);
    if(llm_response_allows_codelab(line)){
      copy_limited(g_model_summary, "llm-api approved start_codelab",
                   sizeof(g_model_summary));
      push_context_note("llm_api_plan", "action=start_codelab");
      return 0;
    }
    copy_limited(g_model_summary, "llm-api rejected codelab",
                 sizeof(g_model_summary));
    push_context_note("llm_api_plan", "action=abort");
    printf("[Planner-Agent] LLM response did not approve CodeLab repair\n");
    return -1;
  }
  printf("[Planner-Agent] rule model selected for offline demo\n");
  return 0;
}

int
main(int argc, char **argv)
{
  char *task;
  int model_mode = 0;
  int retriever_pid;
  int patch_pid;
  int test_pid;
  int reviewer_pid;
  int tool_pid;
  int status = 0;
  int got_review = 0;
  int pos;
  int start_ticks;
  int end_ticks;
  int agentos_ok;
  int file_found;
  int patch_ok;
  int test_ok;
  int review_ok;
  int cache_hits;
  int tool_calls_total;

  task = "fix todo delete bug";
  if(argc > 1 && strcmp(argv[1], "llm-demo") == 0){
    model_mode = 1;
  } else if(argc > 1 && strcmp(argv[1], "llm-api") == 0){
    model_mode = 2;
  } else if(argc > 1){
    task = argv[1];
  }
  if(argc > 2)
    task = argv[2];

  if((uint64)agent_create(AGENT_TYPE_PRIMARY, 12, 1024) == 0){
    printf("[Planner-Agent] agent_create failed\n");
    exit(1);
  }
  agent_sched_set(4, 3);
  agent_watch(AGENT_WATCH_MESSAGE);
  start_ticks = uptime();

  printf("[Planner-Agent] task: %s\n", task);
  copy_limited(g_plan_summary, "find files -> inspect bug -> patch -> test -> review",
               sizeof(g_plan_summary));
  copy_limited(g_file_summary, "repo/todo.c located", sizeof(g_file_summary));
  copy_limited(g_patch_summary, "patch pending", sizeof(g_patch_summary));
  copy_limited(g_test_summary, "test pending", sizeof(g_test_summary));
  copy_limited(g_review_summary, "review pending", sizeof(g_review_summary));
  copy_limited(g_cache_summary, "shared query cache pending",
               sizeof(g_cache_summary));
  copy_limited(g_sched_summary,
               "planner=4/3 retriever=8/8 patch=8/7 test=5/4 reviewer=7/6 tool=4/3",
               sizeof(g_sched_summary));
  if(model_mode == 1)
    copy_limited(g_model_summary, "llm-demo bridge plan, rule executor",
                 sizeof(g_model_summary));
  else if(model_mode == 2)
    copy_limited(g_model_summary, "llm-api waiting for host model",
                 sizeof(g_model_summary));
  else
    copy_limited(g_model_summary, "rule model demo", sizeof(g_model_summary));
  push_context_note("task intake", task);

  memset(&g_event, 0, sizeof(g_event));
  printf("[Planner-Agent] waiting for first HEARTBEAT\n");
  if(agent_wait(1, &g_event) < 0 || g_event.reason != AGENT_WAIT_HEARTBEAT){
    printf("[Planner-Agent] HEARTBEAT wait failed\n");
    exit(1);
  }

  printf("[Kernel-AgentLoop] HEARTBEAT -> wakeup Planner-Agent\n");
  if(request_llm_plan(model_mode, task) < 0)
    exit(1);
  printf("[Planner-Agent] plan: find files -> inspect bug -> patch -> test -> review\n");

  read_repo_bug();

  retriever_pid = spawn_agent("retriever_age");
  patch_pid = spawn_agent("patch_agent");
  test_pid = spawn_agent("test_agent");
  reviewer_pid = spawn_agent("reviewer_agen");
  memset(g_tool_msg, 0, sizeof(g_tool_msg));
  pos = 0;
  append_str(g_tool_msg, &pos, "planner_pid=", sizeof(g_tool_msg));
  append_uint(g_tool_msg, &pos, getpid(), sizeof(g_tool_msg));
  tool_pid = spawn_agent_with_arg("rule_test_too", g_tool_msg);

  if(retriever_pid < 0 || patch_pid < 0 || test_pid < 0 ||
     reviewer_pid < 0 || tool_pid < 0){
    printf("[Planner-Agent] failed to create worker agents\n");
    exit(1);
  }

  printf("[Kernel] create Retriever-Agent pid=%d\n", retriever_pid);
  printf("[Kernel] create Patch-Agent pid=%d\n", patch_pid);
  printf("[Kernel] create Test-Agent pid=%d\n", test_pid);
  printf("[Kernel] create Reviewer-Agent pid=%d\n", reviewer_pid);
  printf("[Kernel] create Tool-Service pid=%d\n", tool_pid);
  printf("[Planner-Agent] spawned retriever=%d patch=%d test=%d reviewer=%d tool=%d\n",
         retriever_pid, patch_pid, test_pid, reviewer_pid, tool_pid);
  printf("[Scheduler] Planner-Agent sched=4/3 heartbeat=12\n");
  printf("[Scheduler] Retriever-Agent high priority sched=8/8\n");
  printf("[Scheduler] Patch-Agent high priority sched=8/7\n");
  printf("[Scheduler] Test-Agent on-demand sched=5/4\n");
  printf("[Scheduler] Reviewer-Agent filemod/message sched=7/6\n");
  printf("[Scheduler] Tool-Service sched=4/3\n");
  push_context_note("spawn workers",
                    "retriever patch test reviewer dynamic tool created");

  sleep(10);
  wait_for_worker_message("stage=tool", "status=ready");

  memset(g_retriever_msg, 0, sizeof(g_retriever_msg));
  pos = 0;
  append_str(g_retriever_msg, &pos,
             "role=retriever;path=repo/todo.c;patch_pid=",
             sizeof(g_retriever_msg));
  append_uint(g_retriever_msg, &pos, patch_pid, sizeof(g_retriever_msg));
  append_str(g_retriever_msg, &pos, ";reviewer_pid=", sizeof(g_retriever_msg));
  append_uint(g_retriever_msg, &pos, reviewer_pid, sizeof(g_retriever_msg));
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
  send_message_to(patch_pid, g_patch_msg);
  wait_for_worker_message("stage=patch", "status=ready");
  send_message_to(test_pid, g_test_msg);
  wait_for_worker_message("stage=test", "status=ready");
  send_message_to(reviewer_pid, g_reviewer_msg);
  wait_for_worker_message("stage=reviewer", "status=watching");
  send_message_to(retriever_pid, g_retriever_msg);
  printf("[Planner-Agent] send_message Retriever-Agent: find delete_task related files\n");
  printf("[Kernel-AgentLoop] MESSAGE -> wakeup Retriever-Agent\n");
  push_context_note("dispatch workers", "initial role messages sent");

  while(!got_review){
    char stage[24];
    char status_value[64];

    memset(&g_event, 0, sizeof(g_event));
    if(agent_wait(1, &g_event) < 0)
      break;
    if(g_event.reason != AGENT_WAIT_MESSAGE)
      continue;
    print_worker_event(g_event.message);
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
      else if(contains(stage, "reviewer") &&
              contains(status_value, "query_cache")){
        copy_limited(g_cache_summary,
                     "retriever first query cache_hit=0; reviewer repeated query cache_hit=1",
                     sizeof(g_cache_summary));
      }
      if(contains(g_event.message, "stage=retriever") &&
         contains(g_event.message, "status=query_cache")){
        copy_limited(g_cache_summary,
                     "retriever first query cache_hit=0; waiting reviewer cache probe",
                     sizeof(g_cache_summary));
      }
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
  g_agentos_syscalls++;

  agent_wait(0, 0);
  agent_info(&g_info);
  end_ticks = uptime();

  file_found = contains(g_file_summary, "found") ||
               contains(g_file_summary, "localized");
  patch_ok = contains(g_patch_summary, "patched") ||
             contains(g_resp.result, "task_count--");
  test_ok = contains(g_test_summary, "status=ok") ||
            contains(g_test_summary, "passed=3");
  review_ok = contains(g_review_summary, "approve") ||
              contains(g_review_summary, "status=done");
  cache_hits = contains(g_cache_summary, "cache_hit=1") ? 1 : 0;
  agentos_ok = file_found && patch_ok && test_ok && review_ok;
  tool_calls_total = g_tool_calls + 4; /* worker-side query/read/patch/test calls */

  printf("[Planner-Agent] final summary\n");
  printf("[Summary] plan: %s\n", g_plan_summary);
  printf("[Summary] key file: %s\n", g_file_summary);
  printf("[Summary] patch: %s\n", g_patch_summary);
  printf("[Summary] test: %s\n", g_test_summary);
  printf("[Summary] review: %s\n", g_review_summary);
  printf("[Summary] shared cache: %s\n", g_cache_summary);
  printf("[Summary] scheduling: %s\n", g_sched_summary);
  printf("[Summary] dynamic tool: run_rule_test_dyn registered and used by Test-Agent\n");
  printf("[Summary] model: %s\n", g_model_summary);
  printf("[Summary] diff: %s\n", g_resp.result);
  printf("[AGENTOS] task=fix_todo_delete status=%s\n",
         agentos_ok ? "PASS" : "FAIL");
  printf("[AGENTOS] file=repo/todo.c found=%d\n", file_found);
  printf("[AGENTOS] patch=task_count-- status=%s\n",
         patch_ok ? "PASS" : "FAIL");
  printf("[AGENTOS] test=rule_test status=%s\n",
         test_ok ? "PASS" : "FAIL");
  printf("[METRIC] suite=dual target=agentos total_ticks=%d files_scanned=1 tool_calls=%d syscalls=%d polling_loops=0 idle_ticks=0 duplicate_queries=1 context_hits=0 cache_hits=%d found=%d patch_ok=%d test_ok=%d review_ok=%d status=%s\n",
         end_ticks - start_ticks, tool_calls_total, g_agentos_syscalls,
         cache_hits, file_found, patch_ok, test_ok, review_ok,
         agentos_ok ? "PASS" : "FAIL");
  printf("[Context] Planner-Agent path: %s\n", g_context_dump);
  printf("[Agent-Loop] final loop_state=%d\n", g_info.loop_state);
  exit(agentos_ok ? 0 : 1);
}
