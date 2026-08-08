#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

#define VALIDATE_BATCH 10000

static int failures;

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
prepare_file(const char *path)
{
  int fd;

  unlink(path);
  fd = open(path, O_CREATE | O_RDWR);
  if(fd < 0)
    return -1;
  if(write(fd, "A context version metric file\n", 30) != 30){
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int
modify_file(const char *path, char marker)
{
  int fd = open(path, O_RDWR);
  int ok;

  if(fd < 0)
    return -1;
  ok = write(fd, &marker, 1) == 1;
  close(fd);
  return ok ? 0 : -1;
}

static int
capture_latest_read(uint64 *sequence, struct agent_context_validation *out)
{
  struct agent_tool_response resp;

  if(call_tool("read_file", "path=ctxvmet", &resp) != AGENT_TOOL_OK)
    return -1;
  memset(out, 0, sizeof(*out));
  if(context_validate(0, out) < 0 || out->state != AGENT_CONTEXT_VALID)
    return -1;
  *sequence = out->sequence;
  return 0;
}

static int
measure_validation(uint64 sequence, int expected_state,
                   struct agent_context_validation *out)
{
  int start = uptime();

  for(int i = 0; i < VALIDATE_BATCH; i++){
    memset(out, 0, sizeof(*out));
    if(context_validate(sequence, out) < 0 || out->state != expected_state)
      return -1;
  }
  return uptime() - start;
}

static void
emit_metric(const char *phase, int run, uint64 sequence,
            struct agent_context_validation *validation, int ticks, int ok)
{
  int delta = (int)(validation->current_version -
                    validation->recorded_version);

  printf("[METRIC] suite=context_version phase=%s run=%d operations=%d ticks=%d sequence=%d recorded_version=%d current_version=%d version_delta=%d state=%d status=%s\n",
         phase, run, VALIDATE_BATCH, ticks, (int)sequence,
         (int)validation->recorded_version,
         (int)validation->current_version, delta, validation->state,
         ok ? "PASS" : "FAIL");
  if(!ok)
    failures++;
}

int
main(int argc, char **argv)
{
  struct agent_context_validation validation;
  uint64 old_sequence;
  uint64 refreshed_sequence;
  int repeats = 5;

  if(argc > 1 && strcmp(argv[1], "small") == 0)
    repeats = 15;
  else if(argc > 1 && strcmp(argv[1], "full") == 0)
    repeats = 30;

  if((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 4096) == 0 ||
     prepare_file("ctxvmet") < 0){
    printf("[SUMMARY] suite=ctxvermetric pass=0 fail=1\n");
    exit(1);
  }

  for(int run = 1; run <= repeats; run++){
    int ticks;
    int ok;

    if(modify_file("ctxvmet", (run & 1) ? 'A' : 'B') < 0 ||
       capture_latest_read(&old_sequence, &validation) < 0){
      failures++;
      continue;
    }
    ticks = measure_validation(old_sequence, AGENT_CONTEXT_VALID, &validation);
    ok = ticks >= 0 &&
         validation.recorded_version == validation.current_version;
    emit_metric("valid_before", run, old_sequence, &validation, ticks, ok);

    if(modify_file("ctxvmet", (run & 1) ? 'B' : 'A') < 0){
      failures++;
      continue;
    }
    ticks = measure_validation(old_sequence, AGENT_CONTEXT_STALE, &validation);
    ok = ticks >= 0 && validation.current_version ==
                         validation.recorded_version + 1;
    emit_metric("stale_after_write", run, old_sequence, &validation,
                ticks, ok);

    if(capture_latest_read(&refreshed_sequence, &validation) < 0){
      failures++;
      continue;
    }
    ticks = measure_validation(refreshed_sequence, AGENT_CONTEXT_VALID,
                               &validation);
    ok = ticks >= 0 &&
         validation.recorded_version == validation.current_version;
    emit_metric("valid_after_reread", run, refreshed_sequence, &validation,
                ticks, ok);
  }

  printf("[SUMMARY] suite=ctxvermetric pass=%d fail=%d\n",
         failures == 0 ? repeats * 3 : 0, failures);
  exit(failures ? 1 : 0);
}
