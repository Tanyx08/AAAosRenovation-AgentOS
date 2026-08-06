#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"

#define MAX_AGENTS 8
#define MAX_WAITS 128

struct sched_result {
  int pid;
  int priority;
  int quota;
  int dispatch;
  int max_wait;
  int p50_wait;
  int p95_wait;
};

static int failures;

static void
sort_ints(int *values, int n)
{
  int i;
  int j;

  for(i = 0; i < n; i++){
    for(j = i + 1; j < n; j++){
      if(values[j] < values[i]){
        int tmp = values[i];

        values[i] = values[j];
        values[j] = tmp;
      }
    }
  }
}

static int
percentile(int *values, int n, int pct)
{
  int idx;

  if(n <= 0)
    return 0;
  sort_ints(values, n);
  idx = (n * pct + 99) / 100 - 1;
  if(idx < 0)
    idx = 0;
  if(idx >= n)
    idx = n - 1;
  return values[idx];
}

static void
busy_work(void)
{
  volatile int x = 0;

  for(int i = 0; i < 4000; i++)
    x += i;
}

static void
sched_worker(int priority, int quota, int runtime_ticks, int result_fd)
{
  struct sched_result result;
  int waits[MAX_WAITS];
  int wait_count = 0;
  int start;
  int last_tick;
  int now;

  memset(&result, 0, sizeof(result));
  memset(waits, 0, sizeof(waits));
  agent_create(AGENT_TYPE_WORKER, 0, 256);
  agent_sched_set(priority, quota);

  result.pid = getpid();
  result.priority = priority;
  result.quota = quota;
  start = uptime();
  last_tick = start;

  while(uptime() - start < runtime_ticks){
    busy_work();
    result.dispatch++;
    now = uptime();
    if(now != last_tick){
      int gap = now - last_tick;

      if(gap > result.max_wait)
        result.max_wait = gap;
      if(wait_count < MAX_WAITS)
        waits[wait_count++] = gap;
      last_tick = now;
    }
  }

  result.p50_wait = percentile(waits, wait_count, 50);
  result.p95_wait = percentile(waits, wait_count, 95);
  write(result_fd, &result, sizeof(result));
  exit(0);
}

static void
set_profile(int agents, int index, int *priority, int *quota)
{
  if(agents == 2){
    if(index == 0){
      *priority = 8;
      *quota = 8;
    } else {
      *priority = 2;
      *quota = 2;
    }
    return;
  }
  if(agents == 4){
    if(index == 0){
      *priority = 8;
      *quota = 8;
    } else if(index == agents - 1){
      *priority = 2;
      *quota = 2;
    } else {
      *priority = 5;
      *quota = 4;
    }
    return;
  }
  if(index < 2){
    *priority = 8;
    *quota = 8;
  } else if(index >= agents - 2){
    *priority = 2;
    *quota = 2;
  } else {
    *priority = 5;
    *quota = 4;
  }
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
run_sched_case(int agents, int runtime_ticks)
{
  struct sched_result results[MAX_AGENTS];
  int pids[MAX_AGENTS];
  int result_pipe[2];
  int priority;
  int quota;
  int status = -1;
  uint64 sum = 0;
  uint64 sum_sq = 0;
  int high_dispatch = 0;
  int low_dispatch = 0;
  int fairness = 0;
  int ok = 1;

  memset(results, 0, sizeof(results));
  if(pipe(result_pipe) < 0){
    printf("[METRIC] suite=scheduler agents=%d fairness=0 high_dispatch=0 low_dispatch=0 status=FAIL\n",
           agents);
    failures++;
    return;
  }

  for(int i = 0; i < agents; i++){
    set_profile(agents, i, &priority, &quota);
    pids[i] = fork();
    if(pids[i] == 0){
      close(result_pipe[0]);
      sched_worker(priority, quota, runtime_ticks, result_pipe[1]);
    }
  }
  close(result_pipe[1]);

  for(int i = 0; i < agents; i++){
    read_exact(result_pipe[0], (char *)&results[i], sizeof(results[i]));
    if(results[i].dispatch <= 0)
      ok = 0;
  }
  close(result_pipe[0]);

  for(int i = 0; i < agents; i++)
    wait(&status);

  for(int i = 0; i < agents; i++){
    int item_ok = results[i].dispatch > 0;

    sum += results[i].dispatch;
    sum_sq += (uint64)results[i].dispatch * results[i].dispatch;
    if(results[i].priority == 8)
      high_dispatch += results[i].dispatch;
    if(results[i].priority == 2)
      low_dispatch += results[i].dispatch;
    printf("[METRIC] suite=scheduler agents=%d pid=%d priority=%d quota=%d dispatch=%d p50_wait=%d p95_wait=%d max_wait=%d status=%s\n",
           agents, results[i].pid, results[i].priority, results[i].quota,
           results[i].dispatch, results[i].p50_wait, results[i].p95_wait,
           results[i].max_wait, item_ok ? "PASS" : "FAIL");
    if(!item_ok)
      failures++;
  }

  if(sum_sq > 0)
    fairness = (sum * sum * 1000) / (agents * sum_sq);
  if(low_dispatch <= 0)
    ok = 0;
  if(high_dispatch < low_dispatch)
    ok = 0;
  if(fairness <= 0)
    ok = 0;

  printf("[METRIC] suite=scheduler agents=%d fairness=%d high_dispatch=%d low_dispatch=%d runtime=%d status=%s\n",
         agents, fairness, high_dispatch, low_dispatch, runtime_ticks,
         ok ? "PASS" : "FAIL");
  if(!ok)
    failures++;
}

int
main(int argc, char *argv[])
{
  int agent_counts[3] = {2, 4, 8};
  int case_count = 1;
  int runtime_ticks = 200;
  char *mode = "quick";

  if(argc > 1 && strcmp(argv[1], "full") == 0){
    case_count = 3;
    runtime_ticks = 500;
    mode = "full";
  } else if(argc > 1 && strcmp(argv[1], "small") == 0){
    case_count = 2;
    runtime_ticks = 300;
    mode = "small";
  }

  if((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 512) == 0){
    printf("[SUMMARY] suite=schedmetric pass=0 fail=1\n");
    exit(1);
  }

  printf("[TEST] suite=schedmetric case=setup mode=%s runtime=%d status=BEGIN\n",
         mode, runtime_ticks);
  for(int i = 0; i < case_count; i++)
    run_sched_case(agent_counts[i], runtime_ticks);

  printf("[SUMMARY] suite=schedmetric pass=%d fail=%d\n",
         failures == 0 ? case_count : 0, failures);
  exit(failures ? 1 : 0);
}
