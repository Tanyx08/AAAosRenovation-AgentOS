# Agent-OS 实现说明

本文档面向项目开发者，目标是帮助你快速理解当前 xv6 Agent-OS 扩展“改了哪里、为什么这样改、每条功能链路怎么跑通”。如果只是想知道怎么使用接口和做演示，请看 `AGENT_OS_INTERFACE_LLM_GUIDE.md`。

当前实现基于 `xv6-2023-mit-labs` 的 `mmap` 分支，新增 Agent 进程、Agent Context 区、结构化工具调用、Context Path 管理，以及面向 Agent 查询模式的 AgentFS 扩展层。

## 1. 文件总览

核心新增和修改文件：

```text
kernel/agent.h       Agent-OS 常量、ABI 结构体、内核函数声明
kernel/agent.c       Agent 核心实现：Context、Tool Call、AgentFS
kernel/sysagent.c    Agent-OS 系统调用入口
kernel/proc.h        PCB 扩展字段
kernel/proc.c        进程生命周期中初始化、清理、fork 继承 Agent 字段
kernel/syscall.h     新增 syscall 编号
kernel/syscall.c     新增 syscall 分发表项
user/user.h          用户态 syscall 声明
user/usys.pl         用户态 syscall stub 生成
user/user.ld         修复用户 ELF 段权限，保证 usertests 通过
user/agenttest.c     Agent-OS 自动测试程序
Makefile             编译 agent.o、sysagent.o、agenttest
```

整体调用路径：

```text
用户态程序
  -> agent_create / tool_call / context_query 等 syscall stub
  -> kernel/syscall.c 分发
  -> kernel/sysagent.c 复制参数并调用 Agent 内核函数
  -> kernel/agent.c 执行 Agent 逻辑
  -> copyout 返回结构化结果或写入 Agent Context 区
```

## 2. Agent ABI 设计

Agent-OS 的公共 ABI 定义在 `kernel/agent.h`。用户态测试程序也直接包含这个头文件：

```c
#include "kernel/agent.h"
```

关键常量：

```c
#define AGENT_CONTEXT_REGION_SIZE (8192)

#define AGENT_TYPE_NORMAL  (0)
#define AGENT_TYPE_PRIMARY (1)
#define AGENT_TYPE_WORKER  (2)

#define AGENT_TOOL_OK                 (0)
#define AGENT_TOOL_ERR_TOOL_NOT_FOUND (-1)
#define AGENT_TOOL_ERR_BAD_PARAM      (-2)
#define AGENT_TOOL_ERR_NOT_AGENT      (-3)
#define AGENT_TOOL_ERR_NO_SPACE       (-4)
```

工具调用请求和响应采用“固定结构体 + 键值对字符串”：

```c
struct agent_tool_request {
  char tool[AGENT_TOOL_NAME_MAX];
  char params[AGENT_TOOL_PARAM_MAX];
};

struct agent_tool_response {
  int status;
  uint32 result_len;
  char result[AGENT_TOOL_RESULT_MAX];
};
```

例如用户态传入：

```text
tool   = "query_file"
params = "type=memory;owner=Agent-B;tags=social;keyword=social"
```

内核返回：

```text
status = AGENT_TOOL_OK
result = "{status=ok,files=[...],count=1,used_index=1,index_scanned=1,full_scanned=3}"
```

这里没有实现 JSON parser，是有意为之：xv6 内核环境很小，使用 `key=value;key=value` 能降低内核字符串解析复杂度，同时仍满足“结构化、可解析、可扩展、有错误处理”的要求。

## 3. PCB 扩展

`kernel/proc.h` 中的 `struct proc` 新增 Agent 字段：

```c
int agent_type;
int heartbeat_interval;
uint64 resource_quota;
int loop_state;
uint64 context_region_start;
uint64 context_region_size;
uint64 context_path_len;
uint64 context_node_count;
uint64 context_dropped_nodes;
uint16 context_offsets[AGENT_CONTEXT_MAX_NODES];
uint16 context_lengths[AGENT_CONTEXT_MAX_NODES];
char agent_message[AGENT_MESSAGE_MAX];
```

字段用途：

```text
agent_type              进程是否为 Agent，以及 Agent 类型
heartbeat_interval      心跳周期，目前作为元信息保存
resource_quota          Context Path 的字节配额
loop_state              Agent Loop 状态
context_region_start    用户态 Agent Context 区起始地址
context_region_size     Agent Context 区大小
context_path_len        当前 Context Path 已使用字节数
context_node_count      当前上下文节点数量
context_dropped_nodes   FIFO 淘汰过的节点数量
context_offsets         每个节点在 Context 区中的偏移
context_lengths         每个节点长度
agent_message           send_message 使用的简单消息槽
```

进程生命周期接入点位于 `kernel/proc.c`：

```c
found:
  p->pid = allocpid();
  p->state = USED;
  agent_init_proc(p);
```

释放进程时重置 Agent 状态：

```c
p->xstate = 0;
agent_init_proc(p);
p->state = UNUSED;
```

`fork()` 时继承 Agent 元信息：

```c
np->sz = p->sz;
agent_after_fork(np, p);
```

因为 xv6 `fork()` 已经通过 `uvmcopy()` 复制用户地址空间，所以 Agent Context 区的用户态内容也会被复制。`agent_after_fork()` 负责同步 PCB 中的 Agent 元信息。

## 4. Agent 进程创建

系统调用入口在 `kernel/sysagent.c`：

```c
uint64
sys_agent_create(void)
{
  int type;
  int heartbeat_interval;
  uint64 quota;

  argint(0, &type);
  argint(1, &heartbeat_interval);
  argaddr(2, &quota);
  return agent_mark_current(type, heartbeat_interval, quota);
}
```

核心实现位于 `kernel/agent.c`：

```c
uint64
agent_mark_current(int type, int heartbeat_interval, uint64 resource_quota)
{
  struct proc *p = myproc();
  uint64 start;
  uint64 end;

  if(type <= AGENT_TYPE_NORMAL)
    type = AGENT_TYPE_PRIMARY;
  if(p->context_region_start == 0){
    start = PGROUNDUP(p->sz);
    end = start + AGENT_CONTEXT_REGION_SIZE;
    if(uvmalloc(p->pagetable, p->sz, end, PTE_W) == 0)
      return -1;
    p->sz = end;
    p->context_region_start = start;
    p->context_region_size = AGENT_CONTEXT_REGION_SIZE;
  }
  p->agent_type = type;
  p->heartbeat_interval = heartbeat_interval;
  p->resource_quota = resource_quota;
  p->loop_state = AGENT_LOOP_READY;
  agent_context_clear(p);
  return p->context_region_start;
}
```

当前版本采用简单稳定的地址空间策略：在当前用户地址空间末尾追加 8192 字节作为 Agent Context 区。这样做的优点是实现小、风险低，并且能让用户态通过返回地址直接读写 Context 区。

地址布局近似如下：

```text
低地址
  text / rodata
  data / bss
  heap
  Agent Context 区
  user stack
高地址
```

注意：当前实现没有把 Agent Context 区做成单独 VMA，而是直接用 `uvmalloc()` 扩展 `p->sz`。后续如果要和普通 mmap 区隔离，可以进一步改为专用 VMA。

## 5. Agent Context 区

Context 区开头是 header：

```c
struct agent_context_header {
  uint32 magic;
  uint32 version;
  uint64 region_size;
  uint64 path_offset;
  uint64 path_length;
  uint64 node_count;
  uint64 dropped_nodes;
  uint64 last_result_len;
  uint64 last_timestamp;
  char last_tool[AGENT_TOOL_NAME_MAX];
  char last_result[AGENT_TOOL_RESULT_MAX];
};
```

当前 `last_tool` 字段是保留字段，主要使用的是 `last_result` 和路径元信息。header 同步函数：

```c
int
agent_sync_header(struct proc *p)
{
  struct agent_context_header hdr;
  char tmp[AGENT_TOOL_RESULT_MAX];
  uint64 n = MIN(p->context_path_len, (uint64)(sizeof(tmp) - 1));

  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = AGENT_CONTEXT_HEADER_MAGIC;
  hdr.version = AGENT_CONTEXT_HEADER_VERSION;
  hdr.region_size = p->context_region_size;
  hdr.path_offset = sizeof(struct agent_context_header);
  hdr.path_length = p->context_path_len;
  hdr.node_count = p->context_node_count;
  hdr.dropped_nodes = p->context_dropped_nodes;
  hdr.last_timestamp = agent_now();
  ...
  return agent_context_write_header(p, &hdr);
}
```

写 header 的本质是一次 `copyout()`：

```c
static int
agent_context_write_header(struct proc *p, struct agent_context_header *hdr)
{
  if(p->context_region_start == 0)
    return -1;
  return copyout(p->pagetable, p->context_region_start, (char*)hdr,
                 sizeof(*hdr));
}
```

这体现了分层存储：

```text
PCB:
  保存可信元信息，例如长度、节点数、配额、淘汰数量

用户态 Context 区:
  保存高频读取的上下文文本和结果缓存
```

用户态直接读 Context 区不需要 syscall；内核只在追加、清空、回滚时同步 header 和元信息。

## 6. Context Path 管理

每次 `tool_call()` 都会自动追加一条上下文节点。节点最终以文本形式写入 Context 区：

```text
{ts=...,req=get_system_status,res={status=ok,procs=...,agents=...,ticks=...}}
```

追加节点函数：

```c
int
agent_context_push_node(struct proc *p, struct agent_context_node *node)
{
  char record[AGENT_CONTEXT_REQ_MAX + AGENT_CONTEXT_RES_MAX + 48];
  ...
  buf_puts(&ptr, &left, "{ts=");
  buf_putu(&ptr, &left, node->timestamp_ms);
  buf_puts(&ptr, &left, ",req=");
  buf_puts(&ptr, &left, node->request);
  buf_puts(&ptr, &left, ",res=");
  buf_puts(&ptr, &left, node->result);
  buf_puts(&ptr, &left, "}\n");
  ...
  while(p->context_node_count >= AGENT_CONTEXT_MAX_NODES ||
        p->context_path_len + rec_len > agent_path_capacity(p))
    agent_evict_oldest(p);
  ...
  copyout(p->pagetable, base + p->context_path_len, record, rec_len);
  p->context_offsets[p->context_node_count] = p->context_path_len;
  p->context_lengths[p->context_node_count] = rec_len;
  p->context_path_len += rec_len;
  p->context_node_count++;
  return agent_sync_header(p);
}
```

配额计算：

```c
static uint64
agent_path_capacity(struct proc *p)
{
  uint64 cap = p->context_region_size - sizeof(struct agent_context_header);

  if(p->resource_quota == 0 || p->resource_quota > cap)
    return cap;
  return p->resource_quota;
}
```

超过配额时执行 FIFO 淘汰：

```c
static void
agent_evict_oldest(struct proc *p)
{
  ...
  first_len = p->context_lengths[0];
  remain = p->context_path_len - first_len;
  copyin(p->pagetable, tmp, base + first_len, remain);
  copyout(p->pagetable, base, tmp, remain);
  ...
  p->context_path_len -= first_len;
  p->context_node_count--;
  p->context_dropped_nodes++;
}
```

回滚逻辑只保留前 `keep_nodes` 个节点：

```c
int
agent_context_rollback(struct proc *p, uint64 keep_nodes)
{
  if(keep_nodes > p->context_node_count)
    return -1;
  if(keep_nodes == 0){
    agent_context_clear(p);
    return 0;
  }
  p->context_path_len = p->context_offsets[keep_nodes - 1] +
                        p->context_lengths[keep_nodes - 1];
  p->context_node_count = keep_nodes;
  p->loop_state = AGENT_LOOP_ROLLED_BACK;
  return agent_sync_header(p);
}
```

## 7. 系统调用封装

`kernel/sysagent.c` 负责处理用户态地址和内核结构体之间的复制。以 `tool_call` 为例：

```c
uint64
sys_tool_call(void)
{
  uint64 ureq;
  uint64 uresp;
  struct agent_tool_request req;
  struct agent_tool_response resp;
  struct proc *p = myproc();

  argaddr(0, &ureq);
  argaddr(1, &uresp);
  if(copyin(p->pagetable, (char*)&req, ureq, sizeof(req)) < 0)
    return -1;
  req.tool[sizeof(req.tool) - 1] = 0;
  req.params[sizeof(req.params) - 1] = 0;
  agent_tool_call(p, &req, &resp);
  if(copyout(p->pagetable, uresp, (char*)&resp, sizeof(resp)) < 0)
    return -1;
  return resp.status;
}
```

几个关键点：

```text
copyin/copyout:
  所有用户指针都通过页表安全复制

强制 NUL 结尾:
  防止用户传入未终止字符串

返回值:
  syscall 返回 resp.status，完整结构化结果写入 resp
```

## 8. Tool Call 分发器

核心函数：

```c
int
agent_tool_call(struct proc *p, struct agent_tool_request *req,
                struct agent_tool_response *resp)
{
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
    tool_set_file_attr(req, resp);
  } else if(streq(req->tool, "get_file_attr")){
    tool_get_file_attr(req, resp);
  } else if(streq(req->tool, "del_file_attr")){
    tool_del_file_attr(req, resp);
  } else if(streq(req->tool, "query_file")){
    tool_query_file(req, resp);
  } else {
    tool_resp_set(resp, AGENT_TOOL_ERR_TOOL_NOT_FOUND, "tool not found");
  }
  ...
  agent_context_push_node(p, &node);
  p->loop_state = AGENT_LOOP_READY;
  return resp->status;
}
```

这段逻辑完成三件事：

```text
1. 检查当前进程必须是 Agent
2. 按 tool 名称分发到内核工具
3. 将请求和结果自动写入 Context Path
```

工具列表：

```text
get_system_status
query_process
send_message
read_context
set_file_attr
get_file_attr
del_file_attr
query_file
```

## 9. 参数解析与结果构造

内核中使用简单的键值对解析函数：

```c
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
    ...
  }
  return -1;
}
```

例如：

```text
params = "path=agentbmem;key=owner;value=Agent-B"
```

调用：

```c
param_value(req->params, "path", path, sizeof(path));
param_value(req->params, "key", key, sizeof(key));
param_value(req->params, "value", value, sizeof(value));
```

结果字符串通过 `buf_puts()`、`buf_putu()` 等小工具安全拼接，避免依赖完整 libc。

## 10. AgentFS 文件查询扩展

AgentFS 是在 xv6 原生文件系统之上的运行时元数据层，没有修改磁盘 inode 格式。

元数据结构：

```c
struct agent_file_attr {
  char key[AGENT_FILE_ATTR_KEY_MAX];
  char value[AGENT_FILE_ATTR_VALUE_MAX];
};

struct agent_file_meta {
  int used;
  uint inum;
  char path[DIRSIZ + 1];
  char summary[AGENT_FILE_SUMMARY_MAX];
  int attr_count;
  struct agent_file_attr attrs[AGENT_FILE_ATTR_MAX];
  int index_next;
};
```

全局表和索引：

```c
static struct spinlock agent_file_lock;
static struct agent_file_meta file_meta[AGENT_FILE_META_MAX];
static int file_index[AGENT_FILE_INDEX_BUCKETS];
```

当前能力：

```text
文件属性系统:
  set_file_attr / get_file_attr / del_file_attr

内容摘要:
  file_summary_refresh() 读取文件前 128 字节

属性索引:
  hash(key=value) -> linked list

结构化查询:
  query_file 返回 path、属性、summary、count、index_scanned、full_scanned
```

设置文件属性时，会校验路径是否存在：

```c
begin_op();
struct inode *ip = namei((char*)path);
if(ip){
  ilock(ip);
  if(ip->type == T_FILE)
    inum = ip->inum;
  iunlockput(ip);
}
end_op();
```

刷新摘要：

```c
static void
file_summary_refresh(struct agent_file_meta *meta)
{
  struct inode *ip;
  int n = 0;

  begin_op();
  ip = namei(meta->path);
  if(ip){
    ilock(ip);
    if(ip->type == T_FILE)
      n = readi(ip, 0, (uint64)meta->summary, 0,
                sizeof(meta->summary) - 1);
    iunlockput(ip);
  }
  end_op();
  if(n < 0)
    n = 0;
  meta->summary[n] = 0;
}
```

哈希索引：

```c
static uint
file_attr_hash(const char *key, const char *value)
{
  uint hash = 5381;
  ...
  return hash % AGENT_FILE_INDEX_BUCKETS;
}
```

当前索引策略比较简单：每个文件使用第一个属性进入哈希桶。`query_file()` 如果有属性条件，就用第一个查询条件定位哈希桶，然后检查剩余条件和 keyword。

查询结果示例：

```text
{status=ok,files=[{path=agentbmem,type=memory,owner=Agent-B,tags=social,summary=social memory alpha: Agent-B met Agent-A}],count=1,used_index=1,index_scanned=1,full_scanned=3}
```

其中：

```text
used_index      是否使用属性索引
index_scanned   索引候选扫描数量
full_scanned    当前元数据表中文件数量
```

这些字段用于展示“按属性查询优于全量遍历”的效果。

## 11. user.ld 修复

本项目修改了 `user/user.ld`，将用户 ELF 拆分为两个 LOAD 段：

```ld
PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}
```

含义：

```text
FLAGS(5) = R + X
FLAGS(6) = R + W
```

`.text` 和 `.rodata` 进入只读可执行段：

```ld
.text : {
  *(.text .text.*)
} :text

.rodata : {
  ...
} :text
```

`.data` 和 `.bss` 进入可写段，并且页对齐：

```ld
. = ALIGN(0x1000);

.data : {
  ...
} :data

.bss : {
  ...
} :data
```

修改原因：原始用户程序被链接成一个 `RWE` LOAD 段，导致地址 `0x0` 的代码页可写。`usertests` 中 `copyout` 测试会执行：

```c
read(fd, (void*)0, 8192);
```

正确行为是内核拒绝向代码页写入。修复后，`exec()` 会根据 ELF flags 为代码页去掉 `PTE_W`，从而通过该测试。

## 12. 测试设计

### 12.1 agenttest

`user/agenttest.c` 是 Agent-OS 功能测试程序，覆盖主线功能。

测试辅助函数：

```c
static int
call_tool(const char *tool, const char *params, struct agent_tool_response *resp)
{
  struct agent_tool_request req;

  memset(&req, 0, sizeof(req));
  strcpy(req.tool, tool);
  strcpy(req.params, params);
  return tool_call(&req, resp);
}
```

核心测试流程：

```text
1. 普通进程调用 tool_call，确认返回 NOT_AGENT
2. 调用 agent_create，确认返回 Context 区地址
3. 调用 agent_info，检查 type、heartbeat、quota、context_start、context_size
4. 直接读取 Agent Context header，检查 magic
5. tool_list 返回 query_file
6. get_system_status 返回 agents 字段
7. query_process type=agent 能查到当前 Agent
8. missing_tool 返回 TOOL_NOT_FOUND
9. 创建 agentbmem / agentplan / agentcfg 三个文件
10. set_file_attr 设置 type / owner / tags
11. get_file_attr 查询 owner
12. query_file 使用 type+owner+tags+keyword 找到 agentbmem
13. 连续 5 次 tool_call，检查 Context Path 自动记录
14. context_query 复制上下文内容
15. context_rollback(2) 回退到两个节点
16. context_clear 清空上下文
17. 小配额 Agent 连续 tool_call，检查 dropped_nodes 增加
```

成功输出：

```text
agenttest: all tests passed
```

### 12.2 usertests

`usertests` 是 xv6 原生回归测试。它验证新增 Agent-OS 模块没有破坏基础内核行为。

当前已验证：

```text
ALL TESTS PASSED
```

### 12.3 mmaptest

因为项目基于 `mmap` 分支，所以还需要验证原始 mmap 实验仍然可用。

当前已验证：

```text
mmaptest: all tests succeeded
```

## 13. 构建与运行

构建：

```bash
make clean
make
make fs.img
```

启动：

```bash
make qemu
```

在 xv6 shell 中运行：

```text
agenttest
usertests
mmaptest
```

退出 QEMU：

```text
Ctrl-A 然后按 X
```

## 14. 当前实现的边界

当前实现已经覆盖基础任务和 AgentFS 查询扩展，但仍有一些边界：

```text
AgentFS 属性表是运行时内存表，重启后不持久化
Context 区由 uvmalloc 追加到用户地址空间末尾，不是独立 VMA
内容摘要是前 128 字节子串匹配，不是 embedding 语义检索
query_file 的索引策略比较简单，只按第一个属性条件走哈希桶
真实 LLM 演示还需要单独的 agent_loop 用户态程序或宿主机桥接脚本
```

这些限制不影响当前实验要求的主线验收。后续可以继续扩展 `.agentmeta` 持久化、专用 VMA、目录递归扫描、多条件索引选择和真实 LLM 串口桥接。
