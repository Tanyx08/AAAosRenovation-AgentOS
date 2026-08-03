#include "kernel/types.h"
#include "kernel/agent.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static int failures;

static void
check(int ok, const char *msg)
{
  if(ok)
    printf("agentorphan: ok: %s\n", msg);
  else {
    printf("agentorphan: FAIL: %s\n", msg);
    failures++;
  }
}

static int
contains(const char *s, const char *needle)
{
  int n = strlen(needle);

  if(n == 0)
    return 1;
  for(; *s; s++){
    int i;
    for(i = 0; i < n && s[i] == needle[i]; i++)
      ;
    if(i == n)
      return 1;
  }
  return 0;
}

static int
query_group(int group, char *buf, int len)
{
  memset(buf, 0, len);
  return agent_query_agent(-1, -1, group, buf, len - 1);
}

static void
worker_wait_forever(int role)
{
  struct agent_wait_event event;

  if((uint64)agent_create(AGENT_TYPE_WORKER, 0, 512) == 0)
    exit(10);
  if(agent_role_set(role) < 0)
    exit(11);
  agent_watch(AGENT_WATCH_MESSAGE);
  memset(&event, 0, sizeof(event));
  agent_wait(1, &event);
  exit(12);
}

static void
explicit_cascade_test(void)
{
  int pids[3];
  int status;

  check((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 1024) > 0,
        "primary workflow created");

  for(int i = 0; i < 3; i++){
    pids[i] = fork();
    if(pids[i] == 0)
      worker_wait_forever(AGENT_ROLE_RETRIEVER + i);
    check(pids[i] > 0, "spawn waiting worker");
  }

  sleep(10);
  check(agent_cascade_kill(AGENT_CASCADE_EXPLICIT) == AGENT_TOOL_OK,
        "explicit cascade kill returns ok");

  for(int i = 0; i < 3; i++){
    status = 0;
    check(wait(&status) == pids[i], "worker reaped after cascade");
    check(status == -1, "worker killed by cascade");
  }
}

static void
auto_planner_exit_worker(void)
{
  int pid = fork();

  if(pid == 0)
    worker_wait_forever(AGENT_ROLE_PATCH);
  if(pid < 0)
    exit(20);
  sleep(10);
  exit(0);
}

static void
auto_cascade_test(void)
{
  int planner;
  int status = 0;
  char agents[AGENT_TOOL_RESULT_MAX];

  check((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 1024) > 0,
        "new workflow after explicit cascade");

  planner = fork();
  if(planner == 0){
    if((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 1024) == 0)
      exit(30);
    auto_planner_exit_worker();
  }
  check(planner > 0, "spawn planner that exits");
  check(wait(&status) == planner && status == 0,
        "planner exits cleanly");

  sleep(30);
  check((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 1024) > 0,
        "monitor agent created");
  check(query_group(planner, agents, sizeof(agents)) > 0,
        "monitor can query planner group");
  check(contains(agents, "count=0"),
        "automatic cascade leaves no orphan agents");
}

int
main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  explicit_cascade_test();
  auto_cascade_test();

  if(failures == 0){
    printf("agentorphan: all tests passed\n");
    exit(0);
  }
  printf("agentorphan: %d failures\n", failures);
  exit(1);
}
