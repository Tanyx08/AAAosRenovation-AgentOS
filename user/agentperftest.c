#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

static int failures;

static void
check(int ok, const char *msg)
{
  if(ok){
    printf("agentperftest: ok: %s\n", msg);
  } else {
    printf("agentperftest: FAIL: %s\n", msg);
    failures++;
  }
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
parse_field(const char *s, const char *field)
{
  int flen = strlen(field);
  int i;

  for(; *s; s++){
    for(i = 0; i < flen && s[i] == field[i]; i++)
      ;
    if(i == flen && s[flen] == '='){
      int value = 0;

      s += flen + 1;
      while(*s >= '0' && *s <= '9'){
        value = value * 10 + *s - '0';
        s++;
      }
      return value;
    }
  }
  return -1;
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
  struct agent_tool_response resp;
  char params[AGENT_TOOL_PARAM_MAX];
  int pos = 0;

  memset(params, 0, sizeof(params));
  append_str(params, &pos, "target_pid=", sizeof(params));
  append_uint(params, &pos, pid, sizeof(params));
  append_str(params, &pos, ";message=", sizeof(params));
  append_str(params, &pos, message, sizeof(params));
  return call_tool("send_message", params, &resp);
}

static void
make_file(const char *path, const char *data)
{
  int fd = open(path, O_CREATE | O_RDWR);

  if(fd < 0){
    failures++;
    return;
  }
  write(fd, data, strlen(data));
  close(fd);
}

static int
set_attr(const char *path, const char *key, const char *value)
{
  struct agent_tool_response resp;
  char params[AGENT_TOOL_PARAM_MAX];
  int pos = 0;

  memset(params, 0, sizeof(params));
  append_str(params, &pos, "path=", sizeof(params));
  append_str(params, &pos, path, sizeof(params));
  append_str(params, &pos, ";key=", sizeof(params));
  append_str(params, &pos, key, sizeof(params));
  append_str(params, &pos, ";value=", sizeof(params));
  append_str(params, &pos, value, sizeof(params));
  return call_tool("set_file_attr", params, &resp);
}

static void
query_file_perf_test(void)
{
  struct agent_tool_response resp_index;
  struct agent_tool_response resp_scan;
  char name[16];
  char content[96];
  char query_index[AGENT_TOOL_PARAM_MAX];
  char query_scan[AGENT_TOOL_PARAM_MAX];
  int idx_scan;
  int idx_full;
  int scan_scan;
  int scan_full;
  int start;
  int batch_index_ticks;
  int batch_scan_ticks;

  for(int i = 0; i < 48; i++){
    memset(name, 0, sizeof(name));
    name[0] = 'p';
    name[1] = 'f';
    name[2] = '0' + (i / 10);
    name[3] = '0' + (i % 10);
    name[4] = 0;

    strcpy(content, "AgentFS perf payload");
    if(i == 17)
      strcpy(content, "AgentFS perf target needle payload");
    make_file(name, content);
    if(i % 4 == 1){
      set_attr(name, "type", "perf");
      set_attr(name, "owner", "Agent-P");
      set_attr(name, "tags", i == 17 ? "hot" : "cold");
    } else {
      set_attr(name, "type", "noise");
      set_attr(name, "owner", "Agent-Q");
      set_attr(name, "tags", "cold");
    }
  }

  strcpy(query_index, "type=perf;owner=Agent-P;tags=hot;keyword=needle");
  strcpy(query_scan,
         "type=perf;owner=Agent-P;tags=hot;keyword=needle;mode=scan");

  check(call_tool("query_file", query_index, &resp_index) == AGENT_TOOL_OK,
        "indexed query_file succeeds");
  check(call_tool("query_file", query_scan, &resp_scan) == AGENT_TOOL_OK,
        "scan query_file succeeds");

  idx_scan = parse_field(resp_index.result, "index_scanned");
  idx_full = parse_field(resp_index.result, "full_scanned");
  scan_scan = parse_field(resp_scan.result, "index_scanned");
  scan_full = parse_field(resp_scan.result, "full_scanned");

  check(idx_scan >= 0 && idx_full >= 0, "indexed query exposes stats");
  check(scan_scan >= 0 && scan_full >= 0, "scan query exposes stats");
  check(idx_scan < idx_full, "index checks fewer files than full scan");
  check(scan_scan == scan_full, "scan mode touches every file");
  check(contains(resp_index.result, "pf17"), "indexed query finds target file");

  start = uptime();
  for(int i = 0; i < 120; i++)
    call_tool("query_file", query_index, &resp_index);
  batch_index_ticks = uptime() - start;

  start = uptime();
  for(int i = 0; i < 120; i++)
    call_tool("query_file", query_scan, &resp_scan);
  batch_scan_ticks = uptime() - start;

  printf("agentperftest: query_file index_scanned=%d full_scanned=%d batch_ticks=%d\n",
         idx_scan, idx_full, batch_index_ticks);
  printf("agentperftest: query_file scan_scanned=%d full_scanned=%d batch_ticks=%d\n",
         scan_scan, scan_full, batch_scan_ticks);
}

static void
heartbeat_latency_test(void)
{
  struct agent_wait_event event;
  int start;
  int reason;

  check(agent_heartbeat_set(6) == 0, "heartbeat latency setup");
  start = uptime();
  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  check((reason & AGENT_EVENT_HEARTBEAT) != 0, "heartbeat wakes wait");
  check(event.tick >= start + 6, "heartbeat waits until interval");
  printf("agentperftest: latency heartbeat_wait_ticks=%d\n",
         (int)(event.tick - start));
  agent_heartbeat_stop();
}

static void
message_latency_test(void)
{
  struct agent_wait_event event;
  char msg[64];
  int parent = getpid();
  int child;
  int status = -1;
  int trigger;
  int reason;
  int pos;

  check(agent_watch(AGENT_WATCH_MESSAGE) == 0, "message latency setup");
  agent_heartbeat_stop();

  child = fork();
  if(child == 0){
    agent_create(AGENT_TYPE_WORKER, 0, 256);
    sleep(6);
    memset(msg, 0, sizeof(msg));
    pos = 0;
    append_str(msg, &pos, "perf-message;trigger=", sizeof(msg));
    append_uint(msg, &pos, uptime(), sizeof(msg));
    if(send_message_to(parent, msg) != AGENT_TOOL_OK)
      exit(1);
    exit(0);
  }

  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  trigger = parse_field(event.message, "trigger");
  check((reason & AGENT_EVENT_MESSAGE) != 0, "message wakes wait");
  check(trigger >= 0 && event.tick >= (uint64)trigger,
        "message latency has trigger timestamp");
  printf("agentperftest: latency message_ticks=%d\n",
         trigger >= 0 ? (int)(event.tick - trigger) : -1);
  check(wait(&status) == child && status == 0, "message latency child exits");
  agent_unwatch(AGENT_WATCH_MESSAGE);
}

static void
filemod_latency_test(void)
{
  struct agent_wait_event event;
  int tick_pipe[2];
  int child;
  int status = -1;
  int fd;
  int trigger = -1;
  int reason;

  fd = open("pflog", O_CREATE | O_RDWR);
  check(fd >= 0, "filemod latency creates file");
  if(fd >= 0)
    close(fd);
  check(pipe(tick_pipe) == 0, "filemod latency creates pipe");
  check(agent_watch_file("pflog") == 0, "filemod latency setup");
  agent_heartbeat_stop();

  child = fork();
  if(child == 0){
    int wfd;
    int now;

    close(tick_pipe[0]);
    sleep(6);
    now = uptime();
    write(tick_pipe[1], &now, sizeof(now));
    wfd = open("pflog", O_RDWR);
    if(wfd < 0)
      exit(1);
    write(wfd, "X", 1);
    close(wfd);
    close(tick_pipe[1]);
    exit(0);
  }

  close(tick_pipe[1]);
  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  read(tick_pipe[0], &trigger, sizeof(trigger));
  close(tick_pipe[0]);
  check((reason & AGENT_EVENT_FILEMOD) != 0, "file modification wakes wait");
  check(trigger >= 0 && event.tick >= (uint64)trigger,
        "filemod latency has trigger timestamp");
  printf("agentperftest: latency filemod_ticks=%d file=%s\n",
         trigger >= 0 ? (int)(event.tick - trigger) : -1, event.file);
  check(wait(&status) == child && status == 0, "filemod latency child exits");
  agent_unwatch(AGENT_WATCH_FILEMOD);
}

static void
idle_sleep_test(void)
{
  struct agent_wait_event event;
  uint64 poll_count = 0;
  int wait_start;
  int wait_ticks;
  int poll_start;

  check(agent_heartbeat_set(20) == 0, "idle sleep heartbeat setup");
  wait_start = uptime();
  memset(&event, 0, sizeof(event));
  agent_wait(1, &event);
  wait_ticks = event.tick - wait_start;
  agent_heartbeat_stop();

  poll_start = uptime();
  while(uptime() - poll_start < wait_ticks)
    poll_count++;

  printf("agentperftest: idle_wait returns=1 wait_ticks=%d polling_loops=%d\n",
         wait_ticks, (int)poll_count);
  check(wait_ticks >= 20, "agent_wait sleeps while idle");
  check(poll_count > 1, "polling loop spins while idle");
}

static void
sched_worker(int write_fd, int priority, int quota, int runtime_ticks)
{
  uint64 start = uptime();
  uint64 counter = 0;

  agent_create(AGENT_TYPE_WORKER, 0, 256);
  if(agent_sched_set(priority, quota) < 0)
    exit(1);
  while(uptime() - start < runtime_ticks){
    counter++;
    if((counter & 0x3ff) == 0)
      ;
  }
  if(write(write_fd, &counter, sizeof(counter)) != sizeof(counter))
    exit(1);
  exit(0);
}

static void
scheduler_perf_test(void)
{
  int high_pipe[2];
  int low_pipe[2];
  int high_child;
  int low_child;
  int status = -1;
  uint64 high_count = 0;
  uint64 low_count = 0;
  int ratio = 0;

  check(pipe(high_pipe) == 0, "scheduler perf high pipe");
  check(pipe(low_pipe) == 0, "scheduler perf low pipe");

  high_child = fork();
  if(high_child == 0){
    close(high_pipe[0]);
    close(low_pipe[0]);
    close(low_pipe[1]);
    sched_worker(high_pipe[1], 8, 8, 70);
  }

  low_child = fork();
  if(low_child == 0){
    close(low_pipe[0]);
    close(high_pipe[0]);
    close(high_pipe[1]);
    sched_worker(low_pipe[1], 2, 1, 70);
  }

  close(high_pipe[1]);
  close(low_pipe[1]);
  check(read(high_pipe[0], &high_count, sizeof(high_count)) == sizeof(high_count),
        "scheduler perf reads high count");
  check(read(low_pipe[0], &low_count, sizeof(low_count)) == sizeof(low_count),
        "scheduler perf reads low count");
  close(high_pipe[0]);
  close(low_pipe[0]);
  check(wait(&status) > 0 && status == 0, "scheduler perf first child exits");
  check(wait(&status) > 0 && status == 0, "scheduler perf second child exits");

  if(low_count > 0)
    ratio = (int)((high_count * 100) / low_count);
  printf("agentperftest: scheduler high_count=%d low_count=%d high_to_low_percent=%d\n",
         (int)high_count, (int)low_count, ratio);
  check(high_count > low_count, "higher priority/quota gets more work");
}

int
main(void)
{
  check((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 1024) > 0,
        "agent_create for performance test");

  query_file_perf_test();
  heartbeat_latency_test();
  message_latency_test();
  filemod_latency_test();
  idle_sleep_test();
  scheduler_perf_test();

  if(failures){
    printf("agentperftest: %d failures\n", failures);
    exit(1);
  }
  printf("agentperftest: all tests passed\n");
  exit(0);
}
