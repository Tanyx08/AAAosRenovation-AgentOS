// Agent 工具调用与动态工具机制。
//
// 决赛改进:
// - #3: capability 权限模型 (每个工具声明 required_cap)
// - #11: tool schema 查询
// - #22: tool_call_batch 批量工具调用

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
extern struct agent_global_state agent_global;

// ---- 内建工具策略表 (修改点 #3) ----
struct agent_tool_policy {
  const char *name;
  uint64 required_cap;
  int group_policy;   // 0=任意, 1=同组
  int mutates_state;
  int audit_required;
};

static struct agent_tool_policy builtin_policies[] = {
  {"query_process",       AGENT_CAP_QUERY_PROCESS,  0, 0, 0},
  {"get_system_status",   AGENT_CAP_QUERY_PROCESS,  0, 0, 0},
  {"send_message",        AGENT_CAP_SEND_MESSAGE,   0, 0, 1},
  {"read_context",        0,                         0, 0, 0},
  {"read_file",           AGENT_CAP_READ_FILE,       0, 0, 0},
  {"patch_file",          AGENT_CAP_PATCH_FILE,      0, 1, 1},
  {"run_rule_test",       AGENT_CAP_QUERY_FILE,      0, 0, 0},
  {"diff_file",           AGENT_CAP_READ_FILE,       0, 0, 0},
  {"set_file_attr",       AGENT_CAP_PATCH_FILE,      0, 1, 1},
  {"get_file_attr",       AGENT_CAP_QUERY_FILE,      0, 0, 0},
  {"del_file_attr",       AGENT_CAP_PATCH_FILE,      0, 1, 1},
  {"query_file",          AGENT_CAP_QUERY_FILE,      0, 0, 0},
  {"query_agent",         AGENT_CAP_QUERY_PROCESS,  0, 0, 0},
  {"get_workflow_metrics", AGENT_CAP_AUDIT_READ,    0, 0, 0},
  {"lease_begin",         AGENT_CAP_LEASE_ACQUIRE,   0, 1, 1},
  {"lease_commit",        AGENT_CAP_LEASE_ACQUIRE,   0, 1, 1},
  {"lease_abort",         AGENT_CAP_LEASE_ACQUIRE,   0, 1, 1},
  {0, 0, 0, 0, 0},
};

struct agent_dynamic_tool {
  int used;
  char name[AGENT_TOOL_NAME_MAX];
  int owner_pid;
  int owner_group;
  int flags;
  uint64 required_cap;     // 修改点 #3: 所需 capability
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
  uint64 span_id;          // 修改点 #19: span ID
  uint64 deadline;         // 修改点 #17: 超时 deadline
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
static int hexval(char c);
static void percent_decode(const char *src, char *dst, int dstsz);
static int str_find(const char *haystack, const char *needle);
static int read_file_content(const char *path, char *buf, int bufsz);

static void
agent_runtime_init(void)
{
  if(agent_runtime_ready)
    return;
  initlock(&agent_runtime_lock, "agent_runtime");
  agent_runtime_ready = 1;
}

static int streq(const char *a, const char *b)
{
  return strncmp(a, b, MAX2(strlen(a), strlen(b)) + 1) == 0;
}

static void buf_putc(char **buf, int *left, char c)
{ if(*left <= 1) return; **buf = c; (*buf)++; (*left)--; **buf = 0; }

static void buf_puts(char **buf, int *left, const char *s)
{ while(*s) buf_putc(buf, left, *s++); }

static void buf_putu(char **buf, int *left, uint64 value)
{
  char tmp[24]; int n = 0;
  if(value == 0){ buf_putc(buf, left, '0'); return; }
  while(value > 0 && n < (int)sizeof(tmp)){ tmp[n++] = '0' + value % 10; value /= 10; }
  while(n > 0) buf_putc(buf, left, tmp[--n]);
}

static int param_value(const char *params, const char *key, char *out, int outsz)
{
  int keylen = strlen(key);
  const char *p = params;
  while(*p){
    if(strncmp(p, key, keylen) == 0 && p[keylen] == '='){
      int n = 0;
      p += keylen + 1;
      while(*p && *p != ';' && n < outsz - 1) out[n++] = *p++;
      out[n] = 0; return 0;
    }
    while(*p && *p != ';') p++;
    if(*p == ';') p++;
  }
  if(outsz > 0) out[0] = 0;
  return -1;
}

static int parse_uint_param(const char *params, const char *key, uint64 *value)
{
  char tmp[32]; uint64 ret = 0;
  if(param_value(params, key, tmp, sizeof(tmp)) < 0 || tmp[0] == 0) return -1;
  for(int i = 0; tmp[i]; i++){
    if(tmp[i] < '0' || tmp[i] > '9') return -1;
    ret = ret * 10 + tmp[i] - '0';
  }
  *value = ret; return 0;
}

static uint64 agent_now(void) { return ticks; }

static int hexval(char c)
{
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void percent_decode(const char *src, char *dst, int dstsz)
{
  int i = 0;
  if(dstsz <= 0) return;
  while(*src && i < dstsz - 1){
    if(src[0] == '%' && src[1] && src[2]){
      int hi = hexval(src[1]), lo = hexval(src[2]);
      if(hi >= 0 && lo >= 0){ dst[i++] = (char)((hi << 4) | lo); src += 3; continue; }
    }
    dst[i++] = (*src == '+') ? ' ' : *src; src++;
  }
  dst[i] = 0;
}

static int str_find(const char *haystack, const char *needle)
{
  int n = strlen(needle);
  if(n == 0) return 0;
  for(int i = 0; haystack[i]; i++){
    int j;
    for(j = 0; j < n && haystack[i + j] == needle[j]; j++);
    if(j == n) return i;
  }
  return -1;
}

static int read_file_content(const char *path, char *buf, int bufsz)
{
  struct inode *ip; int n;
  if(bufsz <= 0) return -1;
  begin_op();
  ip = namei((char*)path);
  if(ip == 0){ end_op(); return -1; }
  ilock(ip);
  if(ip->type != T_FILE){ iunlockput(ip); end_op(); return -1; }
  memset(buf, 0, bufsz);
  n = readi(ip, 0, (uint64)buf, 0, bufsz - 1);
  iunlockput(ip); end_op();
  if(n < 0) return -1;
  buf[n] = 0; return n;
}

static int
read_file_content_versioned(const char *path, char *buf, int bufsz,
                            uint *dev, uint *inum, uint64 *version)
{
  struct inode *ip;
  int n;

  if(bufsz <= 0)
    return -1;
  begin_op();
  ip = namei((char*)path);
  if(ip == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_FILE){
    iunlockput(ip);
    end_op();
    return -1;
  }
  memset(buf, 0, bufsz);
  n = readi(ip, 0, (uint64)buf, 0, bufsz - 1);
  if(n >= 0){
    *dev = ip->dev;
    *inum = ip->inum;
    *version = agentfs_inode_version_get(ip->dev, ip->inum);
  }
  iunlockput(ip);
  end_op();
  if(n < 0)
    return -1;
  buf[n] = 0;
  return n;
}

static void tool_resp_set(struct agent_tool_response *resp, int status, const char *result)
{
  memset(resp, 0, sizeof(*resp));
  resp->status = status;
  safestrcpy(resp->result, result, sizeof(resp->result));
  resp->result_len = strlen(resp->result);
}

static int tool_is_builtin(const char *name)
{
  return streq(name, "query_process") ||
         streq(name, "get_system_status") ||
         streq(name, "send_message") ||
         streq(name, "read_context") ||
         streq(name, "read_file") ||
         streq(name, "patch_file") ||
         streq(name, "run_rule_test") ||
         streq(name, "diff_file") ||
         streq(name, "set_file_attr") ||
         streq(name, "get_file_attr") ||
         streq(name, "del_file_attr") ||
         streq(name, "query_file") ||
         streq(name, "query_agent") ||
         streq(name, "get_workflow_metrics") ||
         streq(name, "lease_begin") ||
         streq(name, "lease_commit") ||
         streq(name, "lease_abort");
}

static int agent_same_group_or_public(struct proc *p, int owner_group, int flags)
{
  return (flags & AGENT_TOOL_FLAG_PUBLIC) ||
         (p->agent_group != 0 && p->agent_group == owner_group);
}

// 修改点 #3: 统一权限检查
int agent_check_tool_permission(struct proc *p, const char *tool_name)
{
  for(int i = 0; builtin_policies[i].name; i++){
    if(streq(builtin_policies[i].name, tool_name)){
      if(builtin_policies[i].required_cap != 0 &&
         !(p->agent_capabilities & builtin_policies[i].required_cap))
        return 0;
      return 1;
    }
  }
  // 动态工具: 检查 group 权限
  agent_runtime_init();
  acquire(&agent_runtime_lock);
  for(int i = 0; i < AGENT_DYNAMIC_TOOL_MAX; i++){
    if(dynamic_tools[i].used && streq(dynamic_tools[i].name, tool_name)){
      int ok = agent_same_group_or_public(p, dynamic_tools[i].owner_group,
                                           dynamic_tools[i].flags);
      release(&agent_runtime_lock);
      return ok;
    }
  }
  release(&agent_runtime_lock);
  return 1; // 工具不存在时由调用者处理
}

// 修改点 #14: 获取工具 schema
int
agent_tool_schema_get(struct proc *p, const char *name,
                       struct agent_tool_schema *schema)
{
  memset(schema, 0, sizeof(*schema));
  safestrcpy(schema->name, name, sizeof(schema->name));

  // 查内建工具
  for(int i = 0; builtin_policies[i].name; i++){
    if(streq(builtin_policies[i].name, name)){
      schema->required_cap = builtin_policies[i].required_cap;
      schema->is_dynamic = 0;
      // 为各内建工具设置参数描述
      if(streq(name, "query_file"))
        safestrcpy(schema->params_desc, "type:string?,owner:string?,tags:string?,keyword:string?,public:string?,mode:string?",
                   sizeof(schema->params_desc));
      else if(streq(name, "send_message"))
        safestrcpy(schema->params_desc, "target_pid:int,message:string",
                   sizeof(schema->params_desc));
      else if(streq(name, "read_file"))
        safestrcpy(schema->params_desc, "path:string",
                   sizeof(schema->params_desc));
      else if(streq(name, "patch_file"))
        safestrcpy(schema->params_desc, "path:string,op:replace,old:string,new:string",
                   sizeof(schema->params_desc));
      else if(streq(name, "run_rule_test"))
        safestrcpy(schema->params_desc, "target:string",
                   sizeof(schema->params_desc));
      else
        safestrcpy(schema->params_desc, "see documentation",
                   sizeof(schema->params_desc));

      safestrcpy(schema->result_desc, "status=int,...",
                 sizeof(schema->result_desc));
      return 0;
    }
  }

  // 查动态工具
  agent_runtime_init();
  acquire(&agent_runtime_lock);
  for(int i = 0; i < AGENT_DYNAMIC_TOOL_MAX; i++){
    if(dynamic_tools[i].used && streq(dynamic_tools[i].name, name)){
      schema->required_cap = dynamic_tools[i].required_cap;
      schema->is_dynamic = 1;
      schema->owner_pid = dynamic_tools[i].owner_pid;
      schema->owner_group = dynamic_tools[i].owner_group;
      schema->flags = dynamic_tools[i].flags;
      safestrcpy(schema->params_desc, "dynamic params",
                 sizeof(schema->params_desc));
      safestrcpy(schema->result_desc, "dynamic result",
                 sizeof(schema->result_desc));
      release(&agent_runtime_lock);
      return 0;
    }
  }
  release(&agent_runtime_lock);
  return -1;
}

// 修改点 #14: 列出带 schema 的工具
int
agent_tool_schema_list(struct proc *p, uint64 dst, uint64 len)
{
  char *tools; char *ptr; int left = AGENT_TOOL_RESULT_MAX; uint64 n;

  tools = kalloc();
  if(tools == 0) return -1;
  ptr = tools;
  memset(tools, 0, AGENT_TOOL_RESULT_MAX);
  buf_puts(&ptr, &left,
    "get_system_status|params=none|cap=QUERY_PROCESS|result=status;"
    "query_process|params=type:string?|cap=QUERY_PROCESS|result=processes[];"
    "query_file|params=type,owner,tags,keyword,public,mode|cap=QUERY_FILE|result=files[];"
    "send_message|params=target_pid,message|cap=SEND_MESSAGE|result=status;"
    "read_context|params=none|cap=none|result=context_text;"
    "read_file|params=path|cap=READ_FILE|result=content;"
    "patch_file|params=path,op,old,new|cap=PATCH_FILE|result=status;"
    "run_rule_test|params=target|cap=QUERY_FILE|result=checks[];"
    "diff_file|params=path|cap=READ_FILE|result=diff;"
    "set_file_attr|params=path,key,value|cap=PATCH_FILE|result=status;"
    "get_file_attr|params=path,key|cap=QUERY_FILE|result=value;"
    "del_file_attr|params=path,key|cap=PATCH_FILE|result=status;"
    "query_agent|params=role,capability,group|cap=QUERY_PROCESS|result=agents[];"
    "lease_begin|params=path|cap=LEASE_ACQUIRE|result=lease_id;"
    "lease_commit|params=lease_id,expected_version|cap=LEASE_ACQUIRE|result=status;"
    "lease_abort|params=lease_id|cap=LEASE_ACQUIRE|result=status");

  agent_runtime_init();
  acquire(&agent_runtime_lock);
  for(int i = 0; i < AGENT_DYNAMIC_TOOL_MAX; i++){
    if(dynamic_tools[i].used &&
       agent_same_group_or_public(p, dynamic_tools[i].owner_group,
                                  dynamic_tools[i].flags)){
      buf_putc(&ptr, &left, ';');
      buf_puts(&ptr, &left, dynamic_tools[i].name);
      buf_puts(&ptr, &left, "|params=dynamic|cap=");
      buf_putu(&ptr, &left, dynamic_tools[i].required_cap);
      buf_puts(&ptr, &left, "|result=dynamic");
    }
  }
  release(&agent_runtime_lock);

  n = MIN((uint64)strlen(tools), len);
  if(copyout(p->pagetable, dst, tools, n) < 0){ kfree(tools); return -1; }
  kfree(tools);
  return n;
}

// ---- 工具实现 ----

static void tool_get_system_status(struct agent_tool_response *resp)
{
  char *buf; char *ptr; int left = AGENT_TOOL_RESULT_MAX;
  int used = 0, agents = 0;
  uint64 heartbeat_scanned, heartbeat_wakeups;
  int heartbeat_active;
  buf = kalloc();
  if(buf == 0){ tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return; }
  ptr = buf; memset(buf, 0, AGENT_TOOL_RESULT_MAX);
  for(struct proc *pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->state != UNUSED){ used++; if(pp->agent_type != AGENT_TYPE_NORMAL) agents++; }
  }
  acquire(&agent_global.lock);
  heartbeat_scanned = agent_global.heartbeat_tick_scanned;
  heartbeat_wakeups = agent_global.heartbeat_tick_wakeups;
  heartbeat_active = agent_global.heartbeat_count;
  release(&agent_global.lock);
  buf_puts(&ptr, &left, "{status=ok,procs="); buf_putu(&ptr, &left, used);
  buf_puts(&ptr, &left, ",agents="); buf_putu(&ptr, &left, agents);
  buf_puts(&ptr, &left, ",ticks="); buf_putu(&ptr, &left, ticks);
  buf_puts(&ptr, &left, ",heartbeat_active="); buf_putu(&ptr, &left, heartbeat_active);
  buf_puts(&ptr, &left, ",heartbeat_scanned="); buf_putu(&ptr, &left, heartbeat_scanned);
  buf_puts(&ptr, &left, ",heartbeat_wakeups="); buf_putu(&ptr, &left, heartbeat_wakeups);
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf); kfree(buf);
}

static void
tool_get_workflow_metrics(struct proc *p, struct agent_tool_response *resp)
{
  struct agent_workflow_metrics metrics;
  char *result;
  char *ptr;
  int left = AGENT_TOOL_RESULT_MAX;

  if(agent_workflow_metrics_get(p, &metrics) != AGENT_TOOL_OK){
    tool_resp_set(resp, AGENT_TOOL_ERR_SERVICE_GONE,
                  "workflow metrics unavailable");
    return;
  }
  result = kalloc();
  if(result == 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory");
    return;
  }
  memset(result, 0, AGENT_TOOL_RESULT_MAX);
  ptr = result;
  buf_puts(&ptr, &left, "{status=ok,workflow_id=");
  buf_putu(&ptr, &left, p->workflow_id);
  buf_puts(&ptr, &left, ",tool_calls=");
  buf_putu(&ptr, &left, metrics.tool_calls);
  buf_puts(&ptr, &left, ",syscalls=");
  buf_putu(&ptr, &left, metrics.syscalls);
  buf_puts(&ptr, &left, ",messages_received=");
  buf_putu(&ptr, &left, metrics.messages_received);
  buf_puts(&ptr, &left, ",query_file_calls=");
  buf_putu(&ptr, &left, metrics.query_file_calls);
  buf_puts(&ptr, &left, ",files_scanned=");
  buf_putu(&ptr, &left, metrics.files_scanned);
  buf_puts(&ptr, &left, ",index_scanned=");
  buf_putu(&ptr, &left, metrics.index_scanned);
  buf_puts(&ptr, &left, ",files_read=");
  buf_putu(&ptr, &left, metrics.files_read);
  buf_puts(&ptr, &left, ",bytes_read=");
  buf_putu(&ptr, &left, metrics.bytes_read);
  buf_puts(&ptr, &left, ",cache_hits=");
  buf_putu(&ptr, &left, metrics.cache_hits);
  buf_puts(&ptr, &left, ",cache_misses=");
  buf_putu(&ptr, &left, metrics.cache_misses);
  buf_puts(&ptr, &left, ",duplicate_queries=");
  buf_putu(&ptr, &left, metrics.duplicate_queries);
  buf_puts(&ptr, &left, ",wait_calls=");
  buf_putu(&ptr, &left, metrics.wait_calls);
  buf_puts(&ptr, &left, ",wait_ticks=");
  buf_putu(&ptr, &left, metrics.wait_ticks);
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, result);
  kfree(result);
}

static void tool_query_process(struct agent_tool_request *req,
                                struct agent_tool_response *resp)
{
  char type[24]; char *buf; char *ptr; int left = AGENT_TOOL_RESULT_MAX;
  int count = 0, only_agent = 0;
  buf = kalloc();
  if(buf == 0){ tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return; }
  ptr = buf; memset(buf, 0, AGENT_TOOL_RESULT_MAX);
  if(param_value(req->params, "type", type, sizeof(type)) == 0 && streq(type, "agent"))
    only_agent = 1;
  buf_puts(&ptr, &left, "{status=ok,processes=[");
  for(struct proc *pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->state == UNUSED) continue;
    if(only_agent && pp->agent_type == AGENT_TYPE_NORMAL) continue;
    if(count > 0) buf_putc(&ptr, &left, ',');
    buf_puts(&ptr, &left, "{pid="); buf_putu(&ptr, &left, pp->pid);
    buf_puts(&ptr, &left, ",name="); buf_puts(&ptr, &left, pp->name);
    buf_puts(&ptr, &left, ",type="); buf_putu(&ptr, &left, pp->agent_type);
    buf_puts(&ptr, &left, ",caps="); buf_putu(&ptr, &left, pp->agent_capabilities);
    buf_putc(&ptr, &left, '}'); count++;
  }
  buf_puts(&ptr, &left, "],count="); buf_putu(&ptr, &left, count);
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf); kfree(buf);
}

static void tool_send_message_tool(struct proc *caller, struct agent_tool_request *req,
                                    struct agent_tool_response *resp)
{
  uint64 pid; char message[AGENT_MESSAGE_MAX]; int message_off, ret;
  if(parse_uint_param(req->params, "target_pid", &pid) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return;
  }
  message_off = str_find(req->params, "message=");
  if(message_off < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return;
  }
  safestrcpy(message, req->params + message_off + strlen("message="), sizeof(message));
  ret = agent_send_message(caller, (int)pid, AGENT_MESSAGE_TYPE_NORMAL,
                           message, 0);
  if(ret == AGENT_TOOL_ERR_BUSY)
    tool_resp_set(resp, ret, "target mailbox full");
  else if(ret != AGENT_TOOL_OK)
    tool_resp_set(resp, ret, "target agent unavailable");
  else
    tool_resp_set(resp, AGENT_TOOL_OK, "message delivered");
}

static void tool_read_context(struct proc *p, struct agent_tool_response *resp)
{
  char *tmp;
  uint64 path_base = p->context_region_start + sizeof(struct agent_context_header);
  uint64 n = MIN((uint64)(AGENT_TOOL_RESULT_MAX - 1), p->context_path_len);
  if(n == 0){ tool_resp_set(resp, AGENT_TOOL_OK, ""); return; }
  tmp = kalloc();
  if(tmp == 0){ tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return; }
  if(copyin(p->pagetable, tmp, path_base + p->context_path_len - n, n) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "context unavailable"); kfree(tmp); return;
  }
  tmp[n] = 0; tool_resp_set(resp, AGENT_TOOL_OK, tmp); kfree(tmp);
}

static void tool_read_file(struct proc *p, struct agent_tool_request *req,
                            struct agent_tool_response *resp)
{
  char path[64], *content, *buf, *ptr;
  int left = AGENT_TOOL_RESULT_MAX, n;
  uint dev, inum;
  uint64 version;
  if(param_value(req->params, "path", path, sizeof(path)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return;
  }
  content = kalloc(); buf = kalloc();
  if(content == 0 || buf == 0){
    if(content) kfree(content);
    if(buf) kfree(buf);
    tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return;
  }
  n = read_file_content_versioned(path, content, AGENT_TOOL_RESULT_MAX,
                                  &dev, &inum, &version);
  if(n < 0){ kfree(content); kfree(buf);
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file not found"); return;
  }
  p->pending_context_dep = 1;
  p->pending_context_dev = dev;
  p->pending_context_inum = inum;
  p->pending_context_version = version;
  agent_workflow_metric_file_read(p, n);
  ptr = buf; memset(buf, 0, AGENT_TOOL_RESULT_MAX);
  buf_puts(&ptr, &left, "{status=ok,path="); buf_puts(&ptr, &left, path);
  buf_puts(&ptr, &left, ",content="); buf_puts(&ptr, &left, content);
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf); kfree(content); kfree(buf);
}

static void tool_patch_file(struct agent_tool_request *req,
                             struct agent_tool_response *resp)
{
  char path[64], op[16], old_enc[AGENT_TOOL_PARAM_MAX], new_enc[AGENT_TOOL_PARAM_MAX];
  char old[AGENT_TOOL_PARAM_MAX], newtext[AGENT_TOOL_PARAM_MAX];
  char *srcbuf, *dstbuf; struct inode *ip;
  int off, size, oldlen, newlen, outlen; uint dev, inum;

  if(param_value(req->params, "path", path, sizeof(path)) < 0 ||
     param_value(req->params, "op", op, sizeof(op)) < 0 ||
     param_value(req->params, "old", old_enc, sizeof(old_enc)) < 0 ||
     param_value(req->params, "new", new_enc, sizeof(new_enc)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return;
  }
  if(!streq(op, "replace")){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "unsupported patch op"); return;
  }
  percent_decode(old_enc, old, sizeof(old));
  percent_decode(new_enc, newtext, sizeof(newtext));
  oldlen = strlen(old); newlen = strlen(newtext);
  if(oldlen == 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "empty old pattern"); return;
  }
  srcbuf = kalloc(); dstbuf = kalloc();
  if(srcbuf == 0 || dstbuf == 0){
    if(srcbuf) kfree(srcbuf);
    if(dstbuf) kfree(dstbuf);
    tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return;
  }

  begin_op();
  ip = namei(path);
  if(ip == 0){ end_op(); kfree(srcbuf); kfree(dstbuf);
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file not found"); return;
  }
  ilock(ip);
  if(ip->type != T_FILE || ip->size >= PGSIZE - 1){
    iunlockput(ip); end_op(); kfree(srcbuf); kfree(dstbuf);
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file too large or invalid"); return;
  }
  size = readi(ip, 0, (uint64)srcbuf, 0, ip->size);
  if(size < 0){ iunlockput(ip); end_op(); kfree(srcbuf); kfree(dstbuf);
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "read failed"); return;
  }
  srcbuf[size] = 0;
  off = str_find(srcbuf, old);
  if(off < 0){ iunlockput(ip); end_op(); kfree(srcbuf); kfree(dstbuf);
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "pattern not found"); return;
  }
  outlen = off + newlen + (size - off - oldlen);
  if(outlen >= PGSIZE - 1){
    iunlockput(ip); end_op(); kfree(srcbuf); kfree(dstbuf);
    tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "patched file too large"); return;
  }
  memmove(dstbuf, srcbuf, off);
  memmove(dstbuf + off, newtext, newlen);
  memmove(dstbuf + off + newlen, srcbuf + off + oldlen, size - off - oldlen);
  dstbuf[outlen] = 0;

  itrunc(ip);
  if(writei(ip, 0, (uint64)dstbuf, 0, outlen) != outlen){
    iunlockput(ip); end_op(); kfree(srcbuf); kfree(dstbuf);
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "write failed"); return;
  }
  memset(ip->summary, 0, sizeof(ip->summary));
  size = readi(ip, 0, (uint64)ip->summary, 0, sizeof(ip->summary) - 1);
  if(size < 0) size = 0;
  ip->summary[size] = 0;
  iupdate(ip);
  dev = ip->dev; inum = ip->inum;
  iunlockput(ip); end_op();

  agentfs_content_changed();
  agent_notify_file_modified(dev, inum);
  kfree(srcbuf); kfree(dstbuf);
  tool_resp_set(resp, AGENT_TOOL_OK, "{status=ok,op=replace}");
}

static void tool_run_rule_test(struct agent_tool_request *req,
                                struct agent_tool_response *resp)
{
  char target[32], *todo, *testsrc, *buf, *ptr;
  int left = AGENT_TOOL_RESULT_MAX, pass_count = 0, total = 3;

  if(param_value(req->params, "target", target, sizeof(target)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return;
  }
  if(!streq(target, "todo_delete")){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "unknown test target"); return;
  }
  todo = kalloc(); testsrc = kalloc(); buf = kalloc();
  if(todo == 0 || testsrc == 0 || buf == 0){
    if(todo) kfree(todo);
    if(testsrc) kfree(testsrc);
    if(buf) kfree(buf);
    tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return;
  }
  if(read_file_content("repo/todo.c", todo, AGENT_TOOL_RESULT_MAX) < 0 ||
     read_file_content("repo/test.c", testsrc, AGENT_TOOL_RESULT_MAX) < 0){
    kfree(todo); kfree(testsrc); kfree(buf);
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "test files unavailable"); return;
  }
  ptr = buf; memset(buf, 0, AGENT_TOOL_RESULT_MAX);
  buf_puts(&ptr, &left, "{status=ok,target=todo_delete,checks=[");
  if(str_find(todo, "task_count--;") >= 0){
    buf_puts(&ptr, &left, "task_count--:PASS"); pass_count++;
  } else { buf_puts(&ptr, &left, "task_count--:FAIL"); }
  buf_putc(&ptr, &left, ',');
  if(str_find(todo, "delete_task") >= 0){
    buf_puts(&ptr, &left, "delete_task:PASS"); pass_count++;
  } else { buf_puts(&ptr, &left, "delete_task:FAIL"); }
  buf_putc(&ptr, &left, ',');
  if(str_find(testsrc, "delete_task") >= 0){
    buf_puts(&ptr, &left, "test_case:PASS"); pass_count++;
  } else { buf_puts(&ptr, &left, "test_case:FAIL"); }
  buf_puts(&ptr, &left, "],passed="); buf_putu(&ptr, &left, pass_count);
  buf_puts(&ptr, &left, ",total="); buf_putu(&ptr, &left, total); buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, pass_count == total ? AGENT_TOOL_OK : AGENT_TOOL_ERR_BAD_PARAM, buf);
  kfree(todo); kfree(testsrc); kfree(buf);
}

static void tool_diff_file(struct agent_tool_request *req,
                            struct agent_tool_response *resp)
{
  char path[64], *content, *buf, *ptr; int left = AGENT_TOOL_RESULT_MAX;
  if(param_value(req->params, "path", path, sizeof(path)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return;
  }
  content = kalloc(); buf = kalloc();
  if(content == 0 || buf == 0){
    if(content) kfree(content);
    if(buf) kfree(buf);
    tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return;
  }
  if(read_file_content(path, content, AGENT_TOOL_RESULT_MAX) < 0){
    kfree(content); kfree(buf);
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file not found"); return;
  }
  ptr = buf; memset(buf, 0, AGENT_TOOL_RESULT_MAX);
  buf_puts(&ptr, &left, "{status=ok,path="); buf_puts(&ptr, &left, path);
  buf_puts(&ptr, &left, ",diff=");
  if(str_find(content, "task_count--;") >= 0)
    buf_puts(&ptr, &left, "- // BUG: missing task_count-- | + task_count--;");
  else if(str_find(content, "missing task_count--") >= 0)
    buf_puts(&ptr, &left, "unchanged bug marker still present");
  else
    buf_puts(&ptr, &left, "no known diff signature");
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf); kfree(content); kfree(buf);
}

static void tool_query_agent(struct proc *caller, struct agent_tool_request *req,
                              struct agent_tool_response *resp)
{
  uint64 role, capability, group;
  char *buf; char *ptr; int left = AGENT_TOOL_RESULT_MAX, count = 0;
  int has_role = (parse_uint_param(req->params, "role", &role) == 0);
  int has_cap = (parse_uint_param(req->params, "capability", &capability) == 0);
  int has_group = (parse_uint_param(req->params, "group", &group) == 0);

  buf = kalloc();
  if(buf == 0){ tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return; }
  ptr = buf; memset(buf, 0, AGENT_TOOL_RESULT_MAX);
  buf_puts(&ptr, &left, "{status=ok,agents=[");

  for(struct proc *pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->state == UNUSED || pp->agent_type == AGENT_TYPE_NORMAL) continue;
    if(has_role && pp->agent_role != (int)role) continue;
    if(has_cap && !(pp->agent_capabilities & (1ULL << (int)capability))) continue;
    if(has_group && pp->agent_group != (int)group) continue;
    if(count > 0) buf_putc(&ptr, &left, ',');
    buf_puts(&ptr, &left, "{pid="); buf_putu(&ptr, &left, pp->pid);
    buf_puts(&ptr, &left, ",role="); buf_putu(&ptr, &left, pp->agent_role);
    buf_puts(&ptr, &left, ",caps="); buf_putu(&ptr, &left, pp->agent_capabilities);
    buf_puts(&ptr, &left, ",group="); buf_putu(&ptr, &left, pp->agent_group);
    buf_puts(&ptr, &left, ",state="); buf_putu(&ptr, &left, pp->loop_state);
    buf_putc(&ptr, &left, '}'); count++;
  }
  buf_puts(&ptr, &left, "],count="); buf_putu(&ptr, &left, count); buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf); kfree(buf);
}

static void tool_lease_begin(struct proc *caller, struct agent_tool_request *req,
                              struct agent_tool_response *resp)
{
  char path[64]; uint64 lease_id, base_version; int ret;
  if(param_value(req->params, "path", path, sizeof(path)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return;
  }
  ret = agent_proc_lease_begin(caller, path, &lease_id, &base_version);
  if(ret != AGENT_TOOL_OK){
    tool_resp_set(resp, ret, "lease begin failed"); return;
  }
  char buf[128]; char *ptr = buf; int left = sizeof(buf);
  memset(buf, 0, sizeof(buf));
  buf_puts(&ptr, &left, "{status=ok,lease_id=");
  buf_putu(&ptr, &left, lease_id);
  buf_puts(&ptr, &left, ",base_version=");
  buf_putu(&ptr, &left, base_version); buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf);
}

static void tool_lease_commit(struct proc *caller, struct agent_tool_request *req,
                               struct agent_tool_response *resp)
{
  uint64 lease_id, expected_version; int ret;
  if(parse_uint_param(req->params, "lease_id", &lease_id) < 0 ||
     parse_uint_param(req->params, "expected_version", &expected_version) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return;
  }
  ret = agent_proc_lease_commit(caller, lease_id, expected_version);
  if(ret == AGENT_TOOL_OK)
    tool_resp_set(resp, AGENT_TOOL_OK, "{status=ok}");
  else if(ret == AGENT_TOOL_ERR_BAD_PARAM)
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "lease stale");
  else
    tool_resp_set(resp, ret, "commit failed");
}

static void tool_lease_abort(struct proc *caller, struct agent_tool_request *req,
                              struct agent_tool_response *resp)
{
  uint64 lease_id; int ret;
  if(parse_uint_param(req->params, "lease_id", &lease_id) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return;
  }
  ret = agent_proc_lease_abort(caller, lease_id);
  if(ret == AGENT_TOOL_OK)
    tool_resp_set(resp, AGENT_TOOL_OK, "{status=ok}");
  else
    tool_resp_set(resp, ret, "abort failed");
}

// ---- 动态工具调用 ----
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
      tool = &dynamic_tools[i]; break;
    }
  }
  if(tool == 0){
    release(&agent_runtime_lock);
    tool_resp_set(resp, AGENT_TOOL_ERR_TOOL_NOT_FOUND, "tool not found"); return;
  }
  // 修改点 #3: capability 检查
  if(tool->required_cap != 0 && !(p->agent_capabilities & tool->required_cap)){
    release(&agent_runtime_lock);
    agent_audit_record(p, tool->owner_pid, req->tool, 0,
                        AGENT_TOOL_ERR_PERMISSION, "insufficient caps");
    tool_resp_set(resp, AGENT_TOOL_ERR_PERMISSION, "permission denied"); return;
  }
  if(!agent_same_group_or_public(p, tool->owner_group, tool->flags)){
    release(&agent_runtime_lock);
    agent_audit_record(p, tool->owner_pid, req->tool, 0,
                        AGENT_TOOL_ERR_PERMISSION, "group mismatch");
    tool_resp_set(resp, AGENT_TOOL_ERR_PERMISSION, "tool permission denied"); return;
  }
  for(int i = 0; i < AGENT_DYNAMIC_REQUEST_MAX; i++){
    if(!dynamic_requests[i].used){ slot = &dynamic_requests[i]; break; }
  }
  if(slot == 0){
    release(&agent_runtime_lock);
    tool_resp_set(resp, AGENT_TOOL_ERR_BUSY, "tool request queue full"); return;
  }

  memset(slot, 0, sizeof(*slot));
  slot->used = 1; slot->id = agent_next_request_id++;
  if(agent_next_request_id <= 0) agent_next_request_id = 1;
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
      tool_resp_set(resp, AGENT_TOOL_ERR_SERVICE_GONE, "caller killed"); return;
    }
    if(slot->id != request_id || !slot->used){
      release(&agent_runtime_lock);
      tool_resp_set(resp, AGENT_TOOL_ERR_SERVICE_GONE, "tool request lost"); return;
    }
    sleep(dynamic_requests, &agent_runtime_lock);
  }
}

// ---- 主工具调用分发 (修改点 #3: 统一权限检查) ----
int
agent_tool_call(struct proc *p, struct agent_tool_request *req,
                struct agent_tool_response *resp)
{
  struct agent_context_node *node;

  if(p->agent_type == AGENT_TYPE_NORMAL){
    tool_resp_set(resp, AGENT_TOOL_ERR_NOT_AGENT, "process is not agent");
    return resp->status;
  }

  // A dependency belongs to exactly one automatically generated Context node.
  // Clearing here prevents a failed prior push from contaminating this call.
  p->pending_context_dep = 0;
  p->pending_context_dev = 0;
  p->pending_context_inum = 0;
  p->pending_context_version = 0;

  // Metrics inspection itself is excluded from business Tool Call totals.
  if(!streq(req->tool, "get_workflow_metrics"))
    agent_workflow_metric_tool_call(p, streq(req->tool, "query_file"));

  // 修改点 #3: 统一权限检查
  if(!agent_check_tool_permission(p, req->tool)){
    agent_audit_record(p, 0, req->tool, 0, AGENT_TOOL_ERR_PERMISSION, "cap check");
    tool_resp_set(resp, AGENT_TOOL_ERR_PERMISSION, "permission denied");
    return resp->status;
  }

  p->loop_state = AGENT_LOOP_RUNNING;

  if(streq(req->tool, "query_process")){
    tool_query_process(req, resp);
  } else if(streq(req->tool, "get_system_status")){
    tool_get_system_status(resp);
  } else if(streq(req->tool, "send_message")){
    tool_send_message_tool(p, req, resp);
  } else if(streq(req->tool, "read_context")){
    tool_read_context(p, resp);
  } else if(streq(req->tool, "read_file")){
    tool_read_file(p, req, resp);
  } else if(streq(req->tool, "patch_file")){
    tool_patch_file(req, resp);
  } else if(streq(req->tool, "run_rule_test")){
    tool_run_rule_test(req, resp);
  } else if(streq(req->tool, "diff_file")){
    tool_diff_file(req, resp);
  } else if(streq(req->tool, "set_file_attr")){
    agentfs_tool_set_file_attr(req, resp);
  } else if(streq(req->tool, "get_file_attr")){
    agentfs_tool_get_file_attr(req, resp);
  } else if(streq(req->tool, "del_file_attr")){
    agentfs_tool_del_file_attr(req, resp);
  } else if(streq(req->tool, "query_file")){
    agentfs_tool_query_file(p, req, resp);
  } else if(streq(req->tool, "query_agent")){
    tool_query_agent(p, req, resp);
  } else if(streq(req->tool, "get_workflow_metrics")){
    tool_get_workflow_metrics(p, resp);
  } else if(streq(req->tool, "lease_begin")){
    tool_lease_begin(p, req, resp);
  } else if(streq(req->tool, "lease_commit")){
    tool_lease_commit(p, req, resp);
  } else if(streq(req->tool, "lease_abort")){
    tool_lease_abort(p, req, resp);
  } else {
    agent_dynamic_tool_call(p, req, resp);
  }

  // 上下文记录
  node = (struct agent_context_node*)kalloc();
  if(node == 0){ p->loop_state = AGENT_LOOP_READY; return resp->status; }
  memset(node, 0, sizeof(*node));
  node->timestamp_ms = agent_now();
  node->status = resp->status;

  safestrcpy(node->request, req->tool, sizeof(node->request));
  if(req->params[0]){
    int n = strlen(node->request);
    if(n < (int)sizeof(node->request) - 2){
      node->request[n] = '(';
      safestrcpy(node->request + n + 1, req->params, sizeof(node->request) - n - 1);
      n = strlen(node->request);
      if(n < (int)sizeof(node->request) - 1){
        node->request[n] = ')'; node->request[n + 1] = 0;
      }
    }
  }
  safestrcpy(node->result, resp->result, sizeof(node->result));
  agent_context_push_node(p, node);

  kfree((void*)node);
  p->loop_state = AGENT_LOOP_READY;
  return resp->status;
}

// 修改点 #22: 批量工具调用
int
agent_tool_call_batch(struct proc *p, struct agent_tool_batch_request *breq,
                       struct agent_tool_batch_response *bresp)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(breq->count < 1 || breq->count > AGENT_TOOL_BATCH_MAX)
    return AGENT_TOOL_ERR_BAD_PARAM;

  bresp->count = 0;
  for(int i = 0; i < breq->count; i++){
    int ret = agent_tool_call(p, &breq->requests[i], &bresp->responses[i]);
    bresp->count++;
    (void)ret;
  }
  return bresp->count;
}

// ---- 动态工具管理 ----
int
agent_tool_register(struct proc *p, const char *name, int flags)
{
  struct agent_dynamic_tool *slot = 0;

  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  // 修改点 #3: 注册工具需要 REGISTER_TOOL capability
  if(!(p->agent_capabilities & AGENT_CAP_REGISTER_TOOL))
    return AGENT_TOOL_ERR_PERMISSION;
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
  if(slot == 0){ release(&agent_runtime_lock); return AGENT_TOOL_ERR_NO_SPACE; }
  memset(slot, 0, sizeof(*slot));
  slot->used = 1;
  safestrcpy(slot->name, name, sizeof(slot->name));
  slot->owner_pid = p->pid;
  slot->owner_group = p->agent_group;
  slot->flags = flags;
  slot->required_cap = 0; // 默认动态工具无额外权限要求
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
        out->span_id = slot->span_id;
        safestrcpy(out->tool, slot->tool, sizeof(out->tool));
        safestrcpy(out->params, slot->params, sizeof(out->params));
        slot->delivered = 1;
        release(&agent_runtime_lock);
        return 0;
      }
    }
    if(killed(p)){ release(&agent_runtime_lock); return -1; }
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
  char *tools; char *ptr; int left = AGENT_TOOL_RESULT_MAX; uint64 n;

  tools = kalloc();
  if(tools == 0) return -1;
  ptr = tools;
  memset(tools, 0, AGENT_TOOL_RESULT_MAX);
  buf_puts(&ptr, &left,
           "get_system_status();query_process(type);"
           "query_file(type,owner,tags,keyword,public,mode);"
           "send_message(target_pid,message);read_context();"
           "read_file(path);patch_file(path,op,old,new);"
           "run_rule_test(target);diff_file(path);"
           "set_file_attr(path,key,value);get_file_attr(path,key);"
           "del_file_attr(path,key);query_agent(role,capability,group);"
           "get_workflow_metrics();"
           "lease_begin(path);lease_commit(lease_id,expected_version);"
           "lease_abort(lease_id)");
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
  if(copyout(p->pagetable, dst, tools, n) < 0){ kfree(tools); return -1; }
  kfree(tools);
  return n;
}

// 修改点 #3/#4: 退出时清理动态工具 + 租约
void
agent_proc_exit(struct proc *p)
{
  agent_heartbeat_wheel_remove(p);

  agent_runtime_init();
  acquire(&agent_runtime_lock);
  for(int i = 0; i < AGENT_DYNAMIC_TOOL_MAX; i++){
    if(dynamic_tools[i].used && dynamic_tools[i].owner_pid == p->pid)
      memset(&dynamic_tools[i], 0, sizeof(dynamic_tools[i]));
  }
  for(int i = 0; i < AGENT_DYNAMIC_REQUEST_MAX; i++){
    struct agent_dynamic_request_slot *slot = &dynamic_requests[i];
    if(!slot->used) continue;
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

  // 修改点 #2: 清理租约
  agent_lease_reap_pid(p->pid);

  // Workflow 成员退出时从内核表摘除；最后一名成员离开后记录自动回收。
  agent_workflow_leave(p);
}
