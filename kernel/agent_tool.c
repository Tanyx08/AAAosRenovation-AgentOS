// Agent 工具调用与动态工具机制。
//
// 这个文件集中实现任务二及相关创新能力：
// 内建工具调用分发、动态工具注册/收发/回复、工具权限控制，
// 以及对用户态 Agent 暴露的工具列表组织。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "agent.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

extern struct proc proc[NPROC];

struct agent_dynamic_tool {
  int used;
  char name[AGENT_TOOL_NAME_MAX];
  int owner_pid;
  int owner_group;
  int flags;
};

struct agent_dynamic_request_slot {
  int used;
  int delivered;
  int replied;
  int id;
  int caller_pid;
  int caller_group;
  int service_pid;
  int status;
  char tool[AGENT_TOOL_NAME_MAX];
  char params[AGENT_TOOL_PARAM_MAX];
  char result[AGENT_TOOL_RESULT_MAX];
};

static struct spinlock agent_runtime_lock;
static int agent_runtime_ready;
static int agent_next_request_id = 1;
static struct agent_dynamic_tool dynamic_tools[AGENT_DYNAMIC_TOOL_MAX];
static struct agent_dynamic_request_slot dynamic_requests[AGENT_DYNAMIC_REQUEST_MAX];

static void agent_runtime_init(void);
static int streq(const char *a, const char *b);
static void buf_putc(char **buf, int *left, char c);
static void buf_puts(char **buf, int *left, const char *s);
static void buf_putu(char **buf, int *left, uint64 value);
static int param_value(const char *params, const char *key, char *out, int outsz);
static int parse_uint_param(const char *params, const char *key, uint64 *value);
static uint64 agent_now(void);

static void
agent_runtime_init(void)
{
  if(agent_runtime_ready)
    return;
  initlock(&agent_runtime_lock, "agent_runtime");
  agent_runtime_ready = 1;
}

static int
streq(const char *a, const char *b)
{
  return strncmp(a, b, MAX2(strlen(a), strlen(b)) + 1) == 0;
}

static void
buf_putc(char **buf, int *left, char c)
{
  if(*left <= 1)
    return;
  **buf = c;
  (*buf)++;
  (*left)--;
  **buf = 0;
}

static void
buf_puts(char **buf, int *left, const char *s)
{
  while(*s)
    buf_putc(buf, left, *s++);
}

static void
buf_putu(char **buf, int *left, uint64 value)
{
  char tmp[24];
  int n = 0;

  if(value == 0){
    buf_putc(buf, left, '0');
    return;
  }
  while(value > 0 && n < sizeof(tmp)){
    tmp[n++] = '0' + value % 10;
    value /= 10;
  }
  while(n > 0)
    buf_putc(buf, left, tmp[--n]);
}

static int
param_value(const char *params, const char *key, char *out, int outsz)
{
  int keylen = strlen(key);
  const char *p = params;

  while(*p){
    if(strncmp(p, key, keylen) == 0 && p[keylen] == '='){
      int n = 0;

      p += keylen + 1;
      while(*p && *p != ';' && n < outsz - 1)
        out[n++] = *p++;
      out[n] = 0;
      return 0;
    }
    while(*p && *p != ';')
      p++;
    if(*p == ';')
      p++;
  }
  if(outsz > 0)
    out[0] = 0;
  return -1;
}

static int
parse_uint_param(const char *params, const char *key, uint64 *value)
{
  char tmp[32];
  uint64 ret = 0;

  if(param_value(params, key, tmp, sizeof(tmp)) < 0 || tmp[0] == 0)
    return -1;
  for(int i = 0; tmp[i]; i++){
    if(tmp[i] < '0' || tmp[i] > '9')
      return -1;
    ret = ret * 10 + tmp[i] - '0';
  }
  *value = ret;
  return 0;
}

static uint64
agent_now(void)
{
  return ticks;
}

static void
tool_resp_set(struct agent_tool_response *resp, int status, const char *result)
{
  memset(resp, 0, sizeof(*resp));
  resp->status = status;
  safestrcpy(resp->result, result, sizeof(resp->result));
  resp->result_len = strlen(resp->result);
}

static int
tool_is_builtin(const char *name)
{
  return streq(name, "query_process") ||
         streq(name, "get_system_status") ||
         streq(name, "send_message") ||
         streq(name, "read_context") ||
         streq(name, "set_file_attr") ||
         streq(name, "get_file_attr") ||
         streq(name, "del_file_attr") ||
         streq(name, "query_file");
}

static int
agent_same_group_or_public(struct proc *p, int owner_group, int flags)
{
  return (flags & AGENT_TOOL_FLAG_PUBLIC) ||
         (p->agent_group != 0 && p->agent_group == owner_group);
}

static void
tool_get_system_status(struct agent_tool_response *resp)
{
  char buf[AGENT_TOOL_RESULT_MAX];
  char *ptr = buf;
  int left = sizeof(buf);
  int used = 0;
  int agents = 0;

  memset(buf, 0, sizeof(buf));
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state != UNUSED){
      used++;
      if(p->agent_type != AGENT_TYPE_NORMAL)
        agents++;
    }
  }
  buf_puts(&ptr, &left, "{status=ok,procs=");
  buf_putu(&ptr, &left, used);
  buf_puts(&ptr, &left, ",agents=");
  buf_putu(&ptr, &left, agents);
  buf_puts(&ptr, &left, ",ticks=");
  buf_putu(&ptr, &left, ticks);
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf);
}

static void
tool_query_process(struct agent_tool_request *req,
                   struct agent_tool_response *resp)
{
  char type[24];
  char buf[AGENT_TOOL_RESULT_MAX];
  char *ptr = buf;
  int left = sizeof(buf);
  int count = 0;
  int only_agent = 0;

  memset(buf, 0, sizeof(buf));
  if(param_value(req->params, "type", type, sizeof(type)) == 0 &&
     streq(type, "agent"))
    only_agent = 1;
  buf_puts(&ptr, &left, "{status=ok,processes=[");
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(only_agent && p->agent_type == AGENT_TYPE_NORMAL)
      continue;
    if(count > 0)
      buf_putc(&ptr, &left, ',');
    buf_puts(&ptr, &left, "{pid=");
    buf_putu(&ptr, &left, p->pid);
    buf_puts(&ptr, &left, ",name=");
    buf_puts(&ptr, &left, p->name);
    buf_puts(&ptr, &left, ",type=");
    buf_putu(&ptr, &left, p->agent_type);
    buf_putc(&ptr, &left, '}');
    count++;
  }
  buf_puts(&ptr, &left, "],count=");
  buf_putu(&ptr, &left, count);
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf);
}

static void
tool_send_message(struct agent_tool_request *req,
                  struct agent_tool_response *resp)
{
  uint64 pid;
  char message[AGENT_MESSAGE_MAX];
  struct proc *target = 0;
  uint64 now;

  if(parse_uint_param(req->params, "target_pid", &pid) < 0 ||
     param_value(req->params, "message", message, sizeof(message)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params");
    return;
  }
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->pid == pid && p->state != UNUSED){
      target = p;
      break;
    }
  }
  if(target == 0 || target->agent_type == AGENT_TYPE_NORMAL){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "target not found");
    return;
  }
  now = ticks;
  acquire(&target->lock);
  safestrcpy(target->agent_message, message, sizeof(target->agent_message));
  if(target->watch_mask & AGENT_WATCH_MESSAGE)
    agent_signal_event_locked(target, AGENT_EVENT_MESSAGE, now);
  release(&target->lock);
  tool_resp_set(resp, AGENT_TOOL_OK, "message delivered");
}

static void
tool_read_context(struct proc *p, struct agent_tool_response *resp)
{
  char tmp[AGENT_TOOL_RESULT_MAX];
  uint64 path_base = p->context_region_start + sizeof(struct agent_context_header);
  uint64 n = MIN((uint64)(sizeof(tmp) - 1), p->context_path_len);

  if(n == 0){
    tool_resp_set(resp, AGENT_TOOL_OK, "");
    return;
  }
  if(copyin(p->pagetable, tmp, path_base + p->context_path_len - n,
            n) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "context unavailable");
    return;
  }
  tmp[n] = 0;
  tool_resp_set(resp, AGENT_TOOL_OK, tmp);
}

static void
agent_dynamic_tool_call(struct proc *p, struct agent_tool_request *req,
                        struct agent_tool_response *resp)
{
  struct agent_dynamic_tool *tool = 0;
  struct agent_dynamic_request_slot *slot = 0;
  int request_id;

  agent_runtime_init();
  acquire(&agent_runtime_lock);
  for(int i = 0; i < AGENT_DYNAMIC_TOOL_MAX; i++){
    if(dynamic_tools[i].used && streq(dynamic_tools[i].name, req->tool)){
      tool = &dynamic_tools[i];
      break;
    }
  }
  if(tool == 0){
    release(&agent_runtime_lock);
    tool_resp_set(resp, AGENT_TOOL_ERR_TOOL_NOT_FOUND, "tool not found");
    return;
  }
  if(!agent_same_group_or_public(p, tool->owner_group, tool->flags)){
    release(&agent_runtime_lock);
    tool_resp_set(resp, AGENT_TOOL_ERR_PERMISSION, "tool permission denied");
    return;
  }
  for(int i = 0; i < AGENT_DYNAMIC_REQUEST_MAX; i++){
    if(!dynamic_requests[i].used){
      slot = &dynamic_requests[i];
      break;
    }
  }
  if(slot == 0){
    release(&agent_runtime_lock);
    tool_resp_set(resp, AGENT_TOOL_ERR_BUSY, "tool request queue full");
    return;
  }

  memset(slot, 0, sizeof(*slot));
  slot->used = 1;
  slot->id = agent_next_request_id++;
  if(agent_next_request_id <= 0)
    agent_next_request_id = 1;
  slot->caller_pid = p->pid;
  slot->caller_group = p->agent_group;
  slot->service_pid = tool->owner_pid;
  safestrcpy(slot->tool, req->tool, sizeof(slot->tool));
  safestrcpy(slot->params, req->params, sizeof(slot->params));
  request_id = slot->id;
  wakeup(dynamic_requests);

  for(;;){
    if(slot->replied){
      tool_resp_set(resp, slot->status, slot->result);
      memset(slot, 0, sizeof(*slot));
      wakeup(dynamic_requests);
      release(&agent_runtime_lock);
      return;
    }
    if(killed(p)){
      memset(slot, 0, sizeof(*slot));
      wakeup(dynamic_requests);
      release(&agent_runtime_lock);
      tool_resp_set(resp, AGENT_TOOL_ERR_SERVICE_GONE, "caller killed");
      return;
    }
    if(slot->id != request_id || !slot->used){
      release(&agent_runtime_lock);
      tool_resp_set(resp, AGENT_TOOL_ERR_SERVICE_GONE, "tool request lost");
      return;
    }
    sleep(dynamic_requests, &agent_runtime_lock);
  }
}

int
agent_tool_call(struct proc *p, struct agent_tool_request *req,
                struct agent_tool_response *resp)
{
  struct agent_context_node node;

  if(p->agent_type == AGENT_TYPE_NORMAL){
    tool_resp_set(resp, AGENT_TOOL_ERR_NOT_AGENT, "process is not agent");
    return resp->status;
  }
  p->loop_state = AGENT_LOOP_RUNNING;
  if(streq(req->tool, "query_process")){
    tool_query_process(req, resp);
  } else if(streq(req->tool, "get_system_status")){
    tool_get_system_status(resp);
  } else if(streq(req->tool, "send_message")){
    tool_send_message(req, resp);
  } else if(streq(req->tool, "read_context")){
    tool_read_context(p, resp);
  } else if(streq(req->tool, "set_file_attr")){
    agentfs_tool_set_file_attr(req, resp);
  } else if(streq(req->tool, "get_file_attr")){
    agentfs_tool_get_file_attr(req, resp);
  } else if(streq(req->tool, "del_file_attr")){
    agentfs_tool_del_file_attr(req, resp);
  } else if(streq(req->tool, "query_file")){
    agentfs_tool_query_file(p, req, resp);
  } else {
    agent_dynamic_tool_call(p, req, resp);
  }

  memset(&node, 0, sizeof(node));
  node.timestamp_ms = agent_now();
  safestrcpy(node.request, req->tool, sizeof(node.request));
  if(req->params[0]){
    int n = strlen(node.request);
    if(n < sizeof(node.request) - 2){
      node.request[n] = '(';
      safestrcpy(node.request + n + 1, req->params,
                 sizeof(node.request) - n - 1);
      n = strlen(node.request);
      if(n < sizeof(node.request) - 1){
        node.request[n] = ')';
        node.request[n + 1] = 0;
      }
    }
  }
  safestrcpy(node.result, resp->result, sizeof(node.result));
  agent_context_push_node(p, &node);
  p->loop_state = AGENT_LOOP_READY;
  return resp->status;
}

int
agent_tool_register(struct proc *p, const char *name, int flags)
{
  struct agent_dynamic_tool *slot = 0;

  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(name[0] == 0 || tool_is_builtin(name) ||
     (flags & ~AGENT_TOOL_FLAG_PUBLIC))
    return AGENT_TOOL_ERR_BAD_PARAM;

  agent_runtime_init();
  acquire(&agent_runtime_lock);
  for(int i = 0; i < AGENT_DYNAMIC_TOOL_MAX; i++){
    if(dynamic_tools[i].used && streq(dynamic_tools[i].name, name)){
      release(&agent_runtime_lock);
      return AGENT_TOOL_ERR_BUSY;
    }
    if(!dynamic_tools[i].used && slot == 0)
      slot = &dynamic_tools[i];
  }
  if(slot == 0){
    release(&agent_runtime_lock);
    return AGENT_TOOL_ERR_NO_SPACE;
  }
  memset(slot, 0, sizeof(*slot));
  slot->used = 1;
  safestrcpy(slot->name, name, sizeof(slot->name));
  slot->owner_pid = p->pid;
  slot->owner_group = p->agent_group;
  slot->flags = flags;
  release(&agent_runtime_lock);
  return 0;
}

int
agent_tool_recv(struct proc *p, struct agent_dynamic_tool_request *out)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;

  agent_runtime_init();
  for(;;){
    acquire(&agent_runtime_lock);
    for(int i = 0; i < AGENT_DYNAMIC_REQUEST_MAX; i++){
      struct agent_dynamic_request_slot *slot = &dynamic_requests[i];

      if(slot->used && !slot->delivered && slot->service_pid == p->pid){
        memset(out, 0, sizeof(*out));
        out->request_id = slot->id;
        out->caller_pid = slot->caller_pid;
        safestrcpy(out->tool, slot->tool, sizeof(out->tool));
        safestrcpy(out->params, slot->params, sizeof(out->params));
        slot->delivered = 1;
        release(&agent_runtime_lock);
        return 0;
      }
    }
    if(killed(p)){
      release(&agent_runtime_lock);
      return -1;
    }
    sleep(dynamic_requests, &agent_runtime_lock);
    release(&agent_runtime_lock);
  }
}

int
agent_tool_reply(struct proc *p, int request_id, const char *result, int status)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;

  agent_runtime_init();
  acquire(&agent_runtime_lock);
  for(int i = 0; i < AGENT_DYNAMIC_REQUEST_MAX; i++){
    struct agent_dynamic_request_slot *slot = &dynamic_requests[i];

    if(slot->used && slot->id == request_id && slot->service_pid == p->pid){
      slot->status = status;
      safestrcpy(slot->result, result, sizeof(slot->result));
      slot->replied = 1;
      wakeup(dynamic_requests);
      release(&agent_runtime_lock);
      return 0;
    }
  }
  release(&agent_runtime_lock);
  return AGENT_TOOL_ERR_BAD_PARAM;
}

int
agent_copy_tool_list(struct proc *p, uint64 dst, uint64 len)
{
  char tools[512];
  char *ptr = tools;
  int left = sizeof(tools);
  uint64 n;

  memset(tools, 0, sizeof(tools));
  buf_puts(&ptr, &left,
           "get_system_status();query_process(type);"
           "send_message(target_pid,message);read_context();"
           "set_file_attr(path,key,value);get_file_attr(path,key);"
           "del_file_attr(path,key);"
           "query_file(type,owner,tags,keyword,public,mode)");
  agent_runtime_init();
  acquire(&agent_runtime_lock);
  for(int i = 0; i < AGENT_DYNAMIC_TOOL_MAX; i++){
    if(dynamic_tools[i].used &&
       agent_same_group_or_public(p, dynamic_tools[i].owner_group,
                                  dynamic_tools[i].flags)){
      buf_putc(&ptr, &left, ';');
      buf_puts(&ptr, &left, dynamic_tools[i].name);
      buf_puts(&ptr, &left, "(dynamic)");
    }
  }
  release(&agent_runtime_lock);
  n = MIN((uint64)strlen(tools), len);

  if(copyout(p->pagetable, dst, tools, n) < 0)
    return -1;
  return n;
}

void
agent_proc_exit(struct proc *p)
{
  agent_runtime_init();
  acquire(&agent_runtime_lock);
  for(int i = 0; i < AGENT_DYNAMIC_TOOL_MAX; i++){
    if(dynamic_tools[i].used && dynamic_tools[i].owner_pid == p->pid)
      memset(&dynamic_tools[i], 0, sizeof(dynamic_tools[i]));
  }
  for(int i = 0; i < AGENT_DYNAMIC_REQUEST_MAX; i++){
    struct agent_dynamic_request_slot *slot = &dynamic_requests[i];

    if(!slot->used)
      continue;
    if(slot->caller_pid == p->pid){
      memset(slot, 0, sizeof(*slot));
    } else if(slot->service_pid == p->pid && !slot->replied){
      slot->status = AGENT_TOOL_ERR_SERVICE_GONE;
      safestrcpy(slot->result, "tool service exited", sizeof(slot->result));
      slot->replied = 1;
    }
  }
  wakeup(dynamic_requests);
  release(&agent_runtime_lock);
}
