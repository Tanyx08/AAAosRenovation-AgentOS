#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

static int failures;

static void
check(int ok, const char *msg)
{
  if(ok){
    printf("agentfsbench: ok: %s\n", msg);
  } else {
    printf("agentfsbench: FAIL: %s\n", msg);
    failures++;
  }
}

static void
make_file(const char *path, const char *data)
{
  int fd = open(path, O_CREATE | O_RDWR);

  if(fd < 0){
    check(0, "create benchmark file");
    return;
  }
  write(fd, data, strlen(data));
  close(fd);
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

static void
append_str(char *dst, int *pos, const char *src, int max)
{
  while(*src && *pos < max - 1)
    dst[(*pos)++] = *src++;
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

static void
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
  check(call_tool("set_file_attr", params, &resp) == AGENT_TOOL_OK,
        "set benchmark attr");
}

int
main(void)
{
  static char *types[] = {"memory", "config", "log", "cache"};
  static char *owners[] = {"Agent-A", "Agent-B", "Agent-C"};
  static char *tags[] = {"social", "plan", "runtime"};
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
  int idx_ticks;
  int scan_ticks;
  int start;
  int batch_index_ticks;
  int batch_scan_ticks;

  check((uint64)agent_create(AGENT_TYPE_PRIMARY, 10, 512) > 0,
        "agent_create for fs bench");

  for(int i = 0; i < 36; i++){
    memset(name, 0, sizeof(name));
    memset(content, 0, sizeof(content));
    name[0] = 'b';
    name[1] = 'f';
    name[2] = '0' + (i / 10);
    name[3] = '0' + (i % 10);
    name[4] = 0;
    strcpy(content, "benchmark payload for AgentFS indexing");
    if(i == 13)
      strcpy(content, "benchmark social target payload for Agent-B");
    make_file(name, content);
    set_attr(name, "type", types[i % 4]);
    set_attr(name, "owner", owners[i % 3]);
    set_attr(name, "tags", tags[i % 3]);
  }

  strcpy(query_index, "type=config;owner=Agent-B;tags=plan;keyword=target");
  strcpy(query_scan,
         "type=config;owner=Agent-B;tags=plan;keyword=target;mode=scan");

  check(call_tool("query_file", query_index, &resp_index) == AGENT_TOOL_OK,
        "indexed query_file");
  check(call_tool("query_file", query_scan, &resp_scan) == AGENT_TOOL_OK,
        "scan query_file");

  idx_scan = parse_field(resp_index.result, "index_scanned");
  idx_full = parse_field(resp_index.result, "full_scanned");
  scan_scan = parse_field(resp_scan.result, "index_scanned");
  scan_full = parse_field(resp_scan.result, "full_scanned");
  idx_ticks = parse_field(resp_index.result, "ticks_cost");
  scan_ticks = parse_field(resp_scan.result, "ticks_cost");

  check(idx_scan >= 0 && idx_full >= 0, "parse indexed query stats");
  check(scan_scan >= 0 && scan_full >= 0, "parse scan query stats");
  check(idx_scan < idx_full, "indexed candidate set smaller than full scan");
  check(scan_scan == scan_full, "scan mode touches all files");

  start = uptime();
  for(int i = 0; i < 200; i++)
    call_tool("query_file", query_index, &resp_index);
  batch_index_ticks = uptime() - start;

  start = uptime();
  for(int i = 0; i < 200; i++)
    call_tool("query_file", query_scan, &resp_scan);
  batch_scan_ticks = uptime() - start;

  printf("agentfsbench: compare indexed(index_scanned=%d, full_scanned=%d, ticks_cost=%d)\n",
         idx_scan, idx_full, idx_ticks);
  printf("agentfsbench: compare fullscan(scanned=%d, full_scanned=%d, ticks_cost=%d)\n",
         scan_scan, scan_full, scan_ticks);
  printf("agentfsbench: batch_ticks indexed=%d fullscan=%d\n",
         batch_index_ticks, batch_scan_ticks);

  if(failures){
    printf("agentfsbench: %d failures\n", failures);
    exit(1);
  }
  printf("agentfsbench: all tests passed\n");
  exit(0);
}
