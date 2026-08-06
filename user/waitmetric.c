#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

static int failures;

static int delays[] = {10, 20, 50};

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
  while(value > 0 && n < (int)sizeof(tmp)){
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
metric_file_name(char *name, char kind, int delay, int run)
{
  int pos = 0;

  memset(name, 0, 16);
  append_str(name, &pos, "wm", 16);
  name[pos++] = kind;
  name[pos] = 0;
  append_uint(name, &pos, delay, 16);
  append_str(name, &pos, "r", 16);
  append_uint(name, &pos, run, 16);
}

static void
write_tick(int fd, int tick)
{
  write(fd, &tick, sizeof(tick));
}

static int
read_tick(int fd)
{
  int tick = -1;

  read(fd, &tick, sizeof(tick));
  return tick;
}

static void
emit_metric(const char *mode, const char *event, int delay, int run,
            int trigger_tick, int handle_tick, int polling_iterations,
            int ok)
{
  int latency = handle_tick - trigger_tick;
  int cpu_ticks = latency >= 0 ? latency : 0;

  if(latency < 0)
    ok = 0;
  printf("[METRIC] suite=wait mode=%s event=%s delay=%d run=%d trigger_tick=%d handle_tick=%d wakeup_latency=%d polling_iterations=%d cpu_ticks=%d status=%s\n",
         mode, event, delay, run, trigger_tick, handle_tick, latency,
         polling_iterations, cpu_ticks, ok ? "PASS" : "FAIL");
  if(!ok)
    failures++;
}

static void
run_message_wait_case(int delay, int run)
{
  struct agent_wait_event event;
  int tick_pipe[2];
  int child;
  int status = -1;
  int trigger_tick;
  int handle_tick;
  int reason;
  int parent_pid = getpid();

  if(pipe(tick_pipe) < 0){
    emit_metric("agent_wait", "message", delay, run, -1, -1, 0, 0);
    return;
  }
  agent_heartbeat_stop();
  agent_watch(AGENT_WATCH_MESSAGE);

  child = fork();
  if(child == 0){
    close(tick_pipe[0]);
    agent_create(AGENT_TYPE_WORKER, 0, 256);
    sleep(delay);
    trigger_tick = uptime();
    write_tick(tick_pipe[1], trigger_tick);
    close(tick_pipe[1]);
    if(send_message_to(parent_pid, "waitmetric-message") != AGENT_TOOL_OK)
      exit(1);
    exit(0);
  }
  close(tick_pipe[1]);

  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  handle_tick = uptime();
  trigger_tick = read_tick(tick_pipe[0]);
  close(tick_pipe[0]);
  wait(&status);
  agent_unwatch(AGENT_WATCH_MESSAGE);

  emit_metric("agent_wait", "message", delay, run, trigger_tick, handle_tick,
              0, reason == AGENT_WAIT_MESSAGE && status == 0);
}

static void
run_message_poll_case(int delay, int run)
{
  char name[16];
  char buf[8];
  int tick_pipe[2];
  int child;
  int status = -1;
  int trigger_tick;
  int handle_tick;
  int fd;
  int polling_iterations = 0;

  metric_file_name(name, 'm', delay, run);
  unlink(name);
  if(pipe(tick_pipe) < 0){
    emit_metric("poll", "message", delay, run, -1, -1, 0, 0);
    return;
  }

  child = fork();
  if(child == 0){
    close(tick_pipe[0]);
    sleep(delay);
    trigger_tick = uptime();
    write_tick(tick_pipe[1], trigger_tick);
    close(tick_pipe[1]);
    fd = open(name, O_CREATE | O_RDWR);
    if(fd < 0)
      exit(1);
    write(fd, "ready", 5);
    close(fd);
    exit(0);
  }
  close(tick_pipe[1]);

  for(;;){
    polling_iterations++;
    fd = open(name, O_RDONLY);
    if(fd >= 0){
      memset(buf, 0, sizeof(buf));
      read(fd, buf, sizeof(buf) - 1);
      close(fd);
      if(strcmp(buf, "ready") == 0)
        break;
    }
  }
  handle_tick = uptime();
  trigger_tick = read_tick(tick_pipe[0]);
  close(tick_pipe[0]);
  wait(&status);
  unlink(name);

  emit_metric("poll", "message", delay, run, trigger_tick, handle_tick,
              polling_iterations, status == 0 && polling_iterations > 0);
}

static void
run_file_wait_case(int delay, int run)
{
  struct agent_wait_event event;
  char name[16];
  int tick_pipe[2];
  int child;
  int status = -1;
  int fd;
  int trigger_tick;
  int handle_tick;
  int reason;

  metric_file_name(name, 'f', delay, run);
  unlink(name);
  fd = open(name, O_CREATE | O_RDWR);
  if(fd >= 0)
    close(fd);
  if(pipe(tick_pipe) < 0){
    emit_metric("agent_wait", "filemod", delay, run, -1, -1, 0, 0);
    return;
  }
  agent_heartbeat_stop();
  agent_watch_file(name);

  child = fork();
  if(child == 0){
    close(tick_pipe[0]);
    sleep(delay);
    trigger_tick = uptime();
    write_tick(tick_pipe[1], trigger_tick);
    close(tick_pipe[1]);
    fd = open(name, O_RDWR);
    if(fd < 0)
      exit(1);
    write(fd, "X", 1);
    close(fd);
    exit(0);
  }
  close(tick_pipe[1]);

  memset(&event, 0, sizeof(event));
  reason = agent_wait(1, &event);
  handle_tick = uptime();
  trigger_tick = read_tick(tick_pipe[0]);
  close(tick_pipe[0]);
  wait(&status);
  agent_unwatch(AGENT_WATCH_FILEMOD);
  unlink(name);

  emit_metric("agent_wait", "filemod", delay, run, trigger_tick, handle_tick,
              0, reason == AGENT_WAIT_FILEMOD && status == 0);
}

static void
run_file_poll_case(int delay, int run)
{
  char name[16];
  char buf[8];
  int tick_pipe[2];
  int child;
  int status = -1;
  int fd;
  int trigger_tick;
  int handle_tick;
  int polling_iterations = 0;

  metric_file_name(name, 'p', delay, run);
  unlink(name);
  fd = open(name, O_CREATE | O_RDWR);
  if(fd >= 0)
    close(fd);
  if(pipe(tick_pipe) < 0){
    emit_metric("poll", "filemod", delay, run, -1, -1, 0, 0);
    return;
  }

  child = fork();
  if(child == 0){
    close(tick_pipe[0]);
    sleep(delay);
    trigger_tick = uptime();
    write_tick(tick_pipe[1], trigger_tick);
    close(tick_pipe[1]);
    fd = open(name, O_RDWR);
    if(fd < 0)
      exit(1);
    write(fd, "Y", 1);
    close(fd);
    exit(0);
  }
  close(tick_pipe[1]);

  for(;;){
    polling_iterations++;
    fd = open(name, O_RDONLY);
    if(fd >= 0){
      memset(buf, 0, sizeof(buf));
      read(fd, buf, sizeof(buf) - 1);
      close(fd);
      if(buf[0] == 'Y')
        break;
    }
  }
  handle_tick = uptime();
  trigger_tick = read_tick(tick_pipe[0]);
  close(tick_pipe[0]);
  wait(&status);
  unlink(name);

  emit_metric("poll", "filemod", delay, run, trigger_tick, handle_tick,
              polling_iterations, status == 0 && polling_iterations > 0);
}

int
main(int argc, char *argv[])
{
  int repeats = 1;
  int scale_count = 1;
  char *mode = "quick";

  if(argc > 1 && strcmp(argv[1], "full") == 0){
    repeats = 30;
    scale_count = 3;
    mode = "full";
  } else if(argc > 1 && strcmp(argv[1], "small") == 0){
    repeats = 3;
    scale_count = 2;
    mode = "small";
  }

  if((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 512) == 0){
    printf("[SUMMARY] suite=waitmetric pass=0 fail=1\n");
    exit(1);
  }

  printf("[TEST] suite=waitmetric case=setup mode=%s repeats=%d status=BEGIN\n",
         mode, repeats);
  for(int i = 0; i < scale_count; i++){
    for(int run = 1; run <= repeats; run++){
      run_message_wait_case(delays[i], run);
      run_message_poll_case(delays[i], run);
      run_file_wait_case(delays[i], run);
      run_file_poll_case(delays[i], run);
    }
  }

  printf("[SUMMARY] suite=waitmetric pass=%d fail=%d\n",
         failures == 0 ? scale_count * repeats * 4 : 0, failures);
  exit(failures ? 1 : 0);
}
