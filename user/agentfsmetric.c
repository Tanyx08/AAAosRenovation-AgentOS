#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

#define SCALE_COUNT 4

static int failures;
static int scales[SCALE_COUNT] = {10, 30, 60, 100};

static void
metric_name(int index, char *name)
{
  name[0] = 'q';
  name[1] = 'm';
  name[2] = '0' + (index / 100);
  name[3] = '0' + ((index / 10) % 10);
  name[4] = '0' + (index % 10);
  name[5] = 0;
}

static void
append_str(char *dst, int *pos, const char *src, int max)
{
  while(*src && *pos < max - 1)
    dst[(*pos)++] = *src++;
  dst[*pos] = 0;
}

static void
append_uint(char *dst, int *pos, uint value, int max)
{
  char tmp[16];
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
make_file(const char *path, int target)
{
  int fd;

  unlink(path);
  fd = open(path, O_CREATE | O_RDWR);
  if(fd < 0){
    printf("[TEST] suite=agentfs case=create_file path=%s status=FAIL\n", path);
    failures++;
    return;
  }
  if(target)
    write(fd, "AgentFS metric target payload keyword needle", 44);
  else
    write(fd, "AgentFS metric background payload", 33);
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
    printf("[TEST] suite=agentfs case=set_attr path=%s key=%s status=FAIL\n",
           path, key);
    failures++;
    return -1;
  }
  return 0;
}

static void
prepare_files(int from, int to)
{
  char name[16];

  for(int i = from; i < to; i++){
    metric_name(i, name);
    make_file(name, i % 10 == 0);
    if(i == 0)
      set_attr(name, "type", "metricperf");
  }
}

static void
rebuild_index_for_scale(int files)
{
  char value[32];
  int pos = 0;

  append_str(value, &pos, "scale_", sizeof(value));
  append_uint(value, &pos, files, sizeof(value));
  set_attr("qm000", "benchver", value);
}

static void
make_query(char *query, int querysz, const char *mode, int files, int run)
{
  int pos = 0;

  memset(query, 0, querysz);
  append_str(query, &pos, "type=metricperf;keyword=needle;public=true", querysz);
  if(strcmp(mode, "scan") == 0)
    append_str(query, &pos, ";mode=scan", querysz);
  append_str(query, &pos, ";nonce=", querysz);
  append_str(query, &pos, mode, querysz);
  append_uint(query, &pos, files, querysz);
  append_str(query, &pos, "r", querysz);
  append_uint(query, &pos, run, querysz);
}

static void
emit_query_metric(const char *mode, int files, int run, const char *query,
                  int expect_cache, int expect_index, int expect_scan)
{
  struct agent_tool_response resp;
  int status;
  int count;
  int scanned;
  int actual_scanned;
  int full_scanned;
  int fs_scanned;
  int ticks;
  int cache_hit;
  int used_index;
  int ok;

  status = call_tool("query_file", query, &resp);
  count = parse_field(resp.result, "count");
  scanned = parse_field(resp.result, "index_scanned");
  full_scanned = parse_field(resp.result, "full_scanned");
  fs_scanned = parse_field(resp.result, "fs_scanned");
  ticks = parse_field(resp.result, "ticks_cost");
  cache_hit = parse_field(resp.result, "cache_hit");
  used_index = parse_field(resp.result, "used_index");
  actual_scanned = cache_hit ? fs_scanned : scanned;

  ok = status == AGENT_TOOL_OK && count > 0 && actual_scanned >= 0 &&
       full_scanned >= 0 && fs_scanned >= 0 && ticks >= 0 &&
       cache_hit == expect_cache;
  if(expect_index)
    ok = ok && used_index == 1 && scanned < full_scanned;
  if(expect_scan)
    ok = ok && used_index == 0 && scanned == full_scanned;
  if(expect_cache)
    ok = ok && fs_scanned == 0;

  printf("[METRIC] suite=agentfs mode=%s files=%d run=%d ticks=%d scanned=%d plan_scanned=%d full_scanned=%d matches=%d cache_hit=%d used_index=%d status=%s\n",
         mode, files, run, ticks, actual_scanned, scanned, full_scanned,
         count, cache_hit, used_index, ok ? "PASS" : "FAIL");

  if(!ok)
    failures++;
}

int
main(int argc, char *argv[])
{
  int created = 0;
  int scale_count = 1;
  int runs = 2;
  const char *profile = "quick";
  char query_index[AGENT_TOOL_PARAM_MAX];
  char query_scan[AGENT_TOOL_PARAM_MAX];
  char query_cache[AGENT_TOOL_PARAM_MAX];
  struct agent_tool_response warm;

  if(argc > 1 && strcmp(argv[1], "full") == 0){
    scale_count = SCALE_COUNT;
    runs = 3;
    profile = "full";
  } else if(argc > 1 && strcmp(argv[1], "small") == 0){
    scale_count = 2;
    profile = "small";
  }

  if((uint64)agent_create(AGENT_TYPE_PRIMARY, 10, 512) == 0){
    printf("[SUMMARY] suite=agentfs pass=0 fail=1\n");
    exit(1);
  }

  printf("[TEST] suite=agentfsmetric case=setup mode=%s status=BEGIN runs=%d\n",
         profile, runs);

  for(int s = 0; s < scale_count; s++){
    int files = scales[s];

    prepare_files(created, files);
    created = files;
    rebuild_index_for_scale(files);

    for(int run = 1; run <= runs; run++){
      make_query(query_scan, sizeof(query_scan), "scan", files, run);
      emit_query_metric("scan", files, run, query_scan, 0, 0, 1);

      make_query(query_index, sizeof(query_index), "index", files, run);
      emit_query_metric("index", files, run, query_index, 0, 1, 0);

      make_query(query_cache, sizeof(query_cache), "cache", files, run);
      call_tool("query_file", query_cache, &warm);
      emit_query_metric("cache", files, run, query_cache, 1, 0, 0);
    }
  }

  printf("[SUMMARY] suite=agentfsmetric pass=%d fail=%d\n",
         failures == 0 ? scale_count * runs * 3 : 0, failures);
  exit(failures ? 1 : 0);
}
