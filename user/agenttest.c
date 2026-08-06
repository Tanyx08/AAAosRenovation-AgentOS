#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
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
    printf("agenttest: ok: %s\n", msg);
  } else {
    printf("agenttest: FAIL: %s\n", msg);
    failures++;
  }
}

static void
make_file(const char *path, const char *data)
{
  int fd = open(path, O_CREATE | O_RDWR);

  check(fd >= 0, "create file");
  if(fd < 0)
    return;
  write(fd, data, strlen(data));
  close(fd);
}

static void
make_exact_file(const char *path, const char *data)
{
  int fd;

  unlink(path);
  fd = open(path, O_CREATE | O_RDWR);
  check(fd >= 0, "create exact file");
  if(fd < 0)
    return;
  write(fd, data, strlen(data));
  close(fd);
}

static int
read_file_into(const char *path, char *buf, int max)
{
  int fd;
  int n;

  if(max <= 0)
    return -1;
  memset(buf, 0, max);
  fd = open(path, O_RDONLY);
  if(fd < 0)
    return -1;
  n = read(fd, buf, max - 1);
  close(fd);
  if(n < 0)
    return -1;
  buf[n] = 0;
  return n;
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
set_attr(const char *params)
{
  struct agent_tool_response resp;

  check(call_tool("set_file_attr", params, &resp) == AGENT_TOOL_OK,
        "set_file_attr");
}

int
main(int argc, char **argv)
{
  struct agent_tool_response resp;
  struct agent_info info;
  struct agent_context_header *hdr;
  char tools[256];
  char tools_after[256];
  char context[768];
  char filebuf[64];
  char *ctx;
  int n;
  int child;
  int status;

  (void)argc;
  (void)argv;

  check(call_tool("get_system_status", "", &resp) == AGENT_TOOL_ERR_NOT_AGENT,
        "normal process rejected by tool_call");

  make_exact_file("capdeny", "before");
  child = fork();
  if(child == 0){
    if(call_tool("patch_file",
                 "path=capdeny;op=replace;old=before;new=after",
                 &resp) == AGENT_TOOL_ERR_NOT_AGENT)
      exit(0);
    exit(1);
  }
  status = 0;
  check(wait(&status) == child && status == 0,
        "normal process rejected by patch_file");
  check(read_file_into("capdeny", filebuf, sizeof(filebuf)) > 0 &&
          strcmp(filebuf, "before") == 0,
        "rejected patch_file leaves file unchanged");

  ctx = (char*)agent_create(AGENT_TYPE_PRIMARY, 25, 1024);
  check((uint64)ctx > 0, "agent_create returns context");
  check(agent_info(&info) == 0, "agent_info syscall");
  check(info.agent_type == AGENT_TYPE_PRIMARY, "agent type initialized");
  check(info.heartbeat_interval == 25, "heartbeat initialized");
  check(info.resource_quota == 1024, "quota initialized");
  check(info.context_start == (uint64)ctx, "context address reported");
  check(info.context_size == AGENT_CONTEXT_REGION_SIZE, "context size reported");

  hdr = (struct agent_context_header*)ctx;
  check(hdr->magic == AGENT_CONTEXT_HEADER_MAGIC, "context header readable");
  check(hdr->version == AGENT_CONTEXT_HEADER_VERSION &&
          hdr->header_size == sizeof(*hdr) &&
          hdr->node_size == sizeof(struct agent_context_node) &&
          (hdr->generation & 1) == 0,
        "context ABI v2 stable header");
  ctx[hdr->path_offset] = 'A';
  ctx[info.context_size - 1] = 'Z';

  child = fork();
  if(child == 0){
    volatile char *guard = ctx + AGENT_CONTEXT_REGION_SIZE;
    *guard = 'X';
    exit(0);
  }
  status = 0;
  check(wait(&status) == child && status == -1,
        "context guard page blocks user write");

  memset(tools, 0, sizeof(tools));
  n = tool_list(tools, sizeof(tools) - 1);
  check(n > 0 && contains(tools, "query_file"), "tool_list reports tools");

  check(call_tool("get_system_status", "", &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "agents="),
        "get_system_status");
  check(call_tool("query_process", "type=agent", &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "type=1"),
        "query_process type=agent");
  check(call_tool("missing_tool", "", &resp) == AGENT_TOOL_ERR_TOOL_NOT_FOUND,
        "missing tool error");

  make_file("agentbmem", "social memory alpha: Agent-B met Agent-A");
  make_file("agentplan", "plan memory beta: Agent-B builds a route");
  make_file("agentcfg", "runtime config gamma");
  set_attr("path=agentbmem;key=type;value=memory");
  set_attr("path=agentbmem;key=owner;value=Agent-B");
  set_attr("path=agentbmem;key=tags;value=social");
  set_attr("path=agentplan;key=type;value=memory");
  set_attr("path=agentplan;key=owner;value=Agent-B");
  set_attr("path=agentplan;key=tags;value=plan");
  set_attr("path=agentcfg;key=type;value=config");
  set_attr("path=agentcfg;key=owner;value=Agent-A");
  set_attr("path=agentcfg;key=tags;value=runtime");

  check(call_tool("get_file_attr", "path=agentbmem;key=owner", &resp) ==
          AGENT_TOOL_OK &&
          contains(resp.result, "owner=Agent-B"),
        "get_file_attr");
  check(call_tool("query_file",
                  "type=memory;owner=Agent-B;tags=social;keyword=social",
                  &resp) == AGENT_TOOL_OK &&
          contains(resp.result, "agentbmem") &&
          contains(resp.result, "used_index=1") &&
          contains(resp.result, "full_scanned="),
        "query_file by attrs and content");

  for(int i = 0; i < 5; i++)
    check(call_tool("get_system_status", "", &resp) == AGENT_TOOL_OK,
          "tool_call records context");
  check(agent_context_verify() == 0, "trusted context digest verifies");

  check(agent_info(&info) == 0 && info.context_node_count >= 5,
        "context records loop nodes");
  hdr = (struct agent_context_header*)info.context_start;
  check(hdr->node_count == info.context_node_count,
        "context header node count");
  memset(context, 0, sizeof(context));
  n = context_query(context, sizeof(context) - 1);
  check(n > 0 && contains(context, "get_system_status"),
        "context_query copies path");
  check(context_rollback(2) == 0, "context_rollback");
  check(agent_info(&info) == 0 && info.context_node_count == 2,
        "rollback keeps requested nodes");
  check(context_clear() == 0, "context_clear");
  check(agent_info(&info) == 0 && info.context_node_count == 0 &&
          info.context_path_len == 0,
        "context clear resets metadata");

  agent_create(AGENT_TYPE_WORKER, 10, 256);
  for(int i = 0; i < 12; i++)
    call_tool("get_system_status", "", &resp);
  check(agent_info(&info) == 0 && info.dropped_nodes > 0,
        "quota eviction drops old nodes");
  check(agent_context_verify() == 0,
        "trusted digest verifies after ring eviction");
  check(agent_role_set(AGENT_ROLE_RETRIEVER) == 0,
        "worker role can be fixed once");
  check(agent_role_set(AGENT_ROLE_PATCH) == AGENT_TOOL_ERR_PERMISSION,
        "worker role cannot be changed after lock");
  check(agent_cap_set(AGENT_CAP_ALL) == AGENT_TOOL_ERR_PERMISSION,
        "worker cannot escalate capabilities");
  memset(tools, 0, sizeof(tools));
  n = tool_list(tools, sizeof(tools) - 1);
  check(n > 0, "tool_list before rejected register");
  check(tool_register("denied_dyn_tool", AGENT_TOOL_FLAG_PUBLIC) ==
          AGENT_TOOL_ERR_PERMISSION,
        "worker cannot register dynamic tool without capability");
  memset(tools_after, 0, sizeof(tools_after));
  n = tool_list(tools_after, sizeof(tools_after) - 1);
  check(n > 0 &&
          !contains(tools_after, "denied_dyn_tool") &&
          contains(tools_after, "query_file"),
        "rejected tool_register leaves dynamic tool table unchanged");

  if(failures){
    printf("agenttest: %d failures\n", failures);
    exit(1);
  }
  printf("agenttest: all tests passed\n");
  exit(0);
}
