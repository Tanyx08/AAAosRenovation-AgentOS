#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"
#include "user/llm_bridge.h"

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
print_demo_response(int request_id)
{
  printf(LLM_RESP_BEGIN " id=%d state=done action_count=1 text=plan_find_patch_test_review action=query_file params=type=code;module=todo;keyword=delete " LLM_RESP_END "\n",
         request_id);
}

int
main(int argc, char **argv)
{
  char *mode = "demo";
  char *prompt = "fix todo delete bug";
  int request_id;
  char line[LLM_RESPONSE_MAX];

  if(argc > 1)
    mode = argv[1];
  if(argc > 2)
    prompt = argv[2];

  if((uint64)agent_create(AGENT_TYPE_WORKER, 0, 512) == 0){
    printf("[LLM-Bridge] agent_create failed\n");
    exit(1);
  }
  agent_sched_set(4, 3);

  request_id = uptime();
  printf("[LLM-Bridge] mode=%s role=planner\n", mode);
  printf(LLM_REQ_BEGIN " id=%d role=planner prompt=%s " LLM_RESP_END "\n",
         request_id, prompt);
  push_context_note("llm_request", prompt);

  if(contains(mode, "demo")){
    print_demo_response(request_id);
    push_context_note("llm_response", "demo plan_find_patch_test_review");
    exit(0);
  }

  printf("[LLM-Bridge] waiting for host proxy response line\n");
  memset(line, 0, sizeof(line));
  gets(line, sizeof(line));
  if(contains(line, LLM_RESP_BEGIN) && contains(line, LLM_RESP_END)){
    printf("[LLM-Bridge] received host response: %s", line);
    push_context_note("llm_response", "host response received");
    exit(0);
  }

  printf("[LLM-Bridge] invalid or missing host response\n");
  push_context_note("llm_response", "host response missing");
  exit(1);
}
