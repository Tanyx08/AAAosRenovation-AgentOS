#include "kernel/types.h"
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
    printf("[CodeLab-Fault] %s status=PASS\n", msg);
  } else {
    printf("[CodeLab-Fault] %s status=FAIL\n", msg);
    failures++;
  }
}

static int
tool_list_contains(const char *name)
{
  char tools[512];
  int n;

  memset(tools, 0, sizeof(tools));
  n = tool_list(tools, sizeof(tools) - 1);
  if(n < 0)
    return 0;
  if(n < sizeof(tools))
    tools[n] = 0;
  return contains(tools, name);
}

static int
call_run_rule_test_dyn(void)
{
  struct agent_tool_request req;
  struct agent_tool_response resp;

  memset(&req, 0, sizeof(req));
  memset(&resp, 0, sizeof(resp));
  strcpy(req.tool, "run_rule_test_dyn");
  strcpy(req.params, "target=todo_delete");
  return tool_call(&req, &resp);
}

int
main(void)
{
  char *argv[] = { "ruletool", 0 };
  int child;
  int status = -1;

  check((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 512) > 0,
        "planner_agent_create");

  child = fork();
  if(child == 0){
    exec("ruletool", argv);
    printf("[CodeLab-Fault] exec ruletool failed\n");
    exit(1);
  }

  sleep(10);
  check(tool_list_contains("run_rule_test_dyn(dynamic)"),
        "dynamic_tool_registered");
  check(kill(child) == 0, "kill_tool_service");
  check(wait(&status) == child, "tool_service_exit");
  check(!tool_list_contains("run_rule_test_dyn(dynamic)"),
        "dynamic_tool_cleanup");
  check(call_run_rule_test_dyn() == AGENT_TOOL_ERR_TOOL_NOT_FOUND,
        "call_after_exit_returns_not_found");

  if(failures){
    printf("[CodeLab-Fault] summary status=FAIL fail=%d\n", failures);
    exit(1);
  }
  printf("[CodeLab-Fault] summary status=PASS fail=0\n");
  exit(0);
}
