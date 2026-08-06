#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

struct metric_result {
  int ok;
  int cache_hit;
  int scanned;
  int plan_scanned;
  int ticks;
  int matches;
};

static int failures;

static void
append_str(char *dst, int *pos, const char *src, int max)
{
  while(*src && *pos < max - 1)
    dst[(*pos)++] = *src++;
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

static void
make_file(const char *path, const char *data)
{
  int fd;

  unlink(path);
  fd = open(path, O_CREATE | O_RDWR);
  if(fd < 0){
    printf("[TEST] suite=context case=create_file path=%s status=FAIL\n", path);
    failures++;
    return;
  }
  write(fd, data, strlen(data));
  close(fd);
}

static int
set_attr(const char *path, const char *key, const char *value)
{
  char params[AGENT_TOOL_PARAM_MAX];
  int pos = 0;
  struct agent_tool_response resp;

  memset(params, 0, sizeof(params));
  append_str(params, &pos, "path=", sizeof(params));
  append_str(params, &pos, path, sizeof(params));
  append_str(params, &pos, ";key=", sizeof(params));
  append_str(params, &pos, key, sizeof(params));
  append_str(params, &pos, ";value=", sizeof(params));
  append_str(params, &pos, value, sizeof(params));

  if(call_tool("set_file_attr", params, &resp) != AGENT_TOOL_OK){
    printf("[TEST] suite=context case=set_attr path=%s key=%s status=FAIL\n",
           path, key);
    failures++;
    return -1;
  }
  return 0;
}

static struct metric_result
run_query(const char *query, int expected_cache_hit)
{
  struct metric_result out;
  struct agent_tool_response resp;
  int status;
  int fs_scanned;

  memset(&out, 0, sizeof(out));
  status = call_tool("query_file", query, &resp);
  out.cache_hit = parse_field(resp.result, "cache_hit");
  out.plan_scanned = parse_field(resp.result, "index_scanned");
  fs_scanned = parse_field(resp.result, "fs_scanned");
  out.ticks = parse_field(resp.result, "ticks_cost");
  out.matches = parse_field(resp.result, "count");
  out.scanned = out.cache_hit ? fs_scanned : out.plan_scanned;
  out.ok = status == AGENT_TOOL_OK &&
           out.cache_hit == expected_cache_hit &&
           out.scanned >= 0 &&
           out.ticks >= 0 &&
           out.matches >= 0;
  return out;
}

static void
emit_metric(const char *phase, struct metric_result r, int saved_ticks,
            int expect_matches)
{
  int ok = r.ok && r.matches == expect_matches;

  printf("[METRIC] suite=context phase=%s cache_hit=%d context_hit=0 tool_calls=1 query_file_calls=1 scanned=%d plan_scanned=%d ticks=%d saved_ticks=%d matches=%d status=%s\n",
         phase, r.cache_hit, r.scanned, r.plan_scanned, r.ticks,
         saved_ticks, r.matches, ok ? "PASS" : "FAIL");
  if(!ok)
    failures++;
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

int
main(void)
{
  const char *old_query =
    "type=ctxmetric;owner=system;tags=repeat;public=true;keyword=context";
  struct metric_result first;
  struct metric_result cached;
  struct metric_result invalidated;
  int result_pipe[2];
  int child;
  int status = -1;
  int saved_ticks;

  if((uint64)agent_create(AGENT_TYPE_PRIMARY, 10, 512) == 0){
    printf("[SUMMARY] suite=context pass=0 fail=1\n");
    exit(1);
  }

  printf("[TEST] suite=contextmetric case=setup status=BEGIN\n");
  make_file("ctxmet", "context metric shared cache repeat sample");
  set_attr("ctxmet", "type", "ctxmetric");
  set_attr("ctxmet", "owner", "system");
  set_attr("ctxmet", "tags", "repeat");

  first = run_query(old_query, 0);
  emit_metric("first", first, 0, 1);

  pipe(result_pipe);
  child = fork();
  if(child == 0){
    agent_create(AGENT_TYPE_WORKER, 0, 256);
    cached = run_query(old_query, 1);
    write(result_pipe[1], &cached, sizeof(cached));
    exit(cached.ok && cached.matches == 1 ? 0 : 1);
  }
  read_exact(result_pipe[0], (char *)&cached, sizeof(cached));
  if(wait(&status) != child || status != 0)
    cached.ok = 0;
  saved_ticks = first.ticks > cached.ticks ? first.ticks - cached.ticks : 0;
  emit_metric("cache_hit", cached, saved_ticks, 1);

  set_attr("ctxmet", "tags", "updated");
  invalidated = run_query(old_query, 0);
  emit_metric("invalidated", invalidated, 0, 0);

  printf("[SUMMARY] suite=contextmetric pass=%d fail=%d\n",
         failures == 0 ? 3 : 0, failures);
  exit(failures ? 1 : 0);
}
