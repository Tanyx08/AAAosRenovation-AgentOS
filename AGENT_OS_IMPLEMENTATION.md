# Agent-OS 实现说明

本文档面向项目开发者和答辩检查人员，说明当前 xv6 Agent-OS 扩展完成了什么、代码改在哪里、每条功能链路如何运行，以及五个赛题任务对应到哪些实现。

当前实现基于 `xv6-2023-mit-labs` 的 `mmap` 分支，新增了 Agent 进程模型、用户态 Agent Context 区、结构化 Tool Call、Context Path 管理、AgentFS 查询扩展，以及 Agent Loop 的心跳/事件唤醒机制。

## 1. 完成度总览

| 任务 | 完成度 | 说明 |
| --- | --- | --- |
| 任务一：Agent 进程创建与地址空间设计 | 已完成 | PCB 扩展、`agent_create`、`agent_info`、用户态 Agent Context 区均已实现。 |
| 任务二：结构化交互接口 | 已完成 | `tool_call`/`tool_list` 已实现，内核提供 8 个工具，包含错误码和结构化响应。 |
| 任务三：上下文路径管理 | 已完成 | Context Path 分层存储、自动追加、查询、回滚、清空、FIFO 淘汰均已实现。 |
| 任务四：Agent 查询优化文件系统扩展 | 已完成主要要求 | 实现文件属性、内容摘要、属性哈希索引、结构化查询结果和扫描统计。 |
| 任务五：Agent Loop 内核运行机制 | 已完成扩展版 | 实现心跳、消息事件、文件修改事件、`agent_wait` 休眠/唤醒、Loop 生命周期，以及带优先级/配额的多 Agent 调度。 |

当前没有实现的增强项主要是：AgentFS 复杂语义检索、多文件条件 watch、消息队列和真实 LLM 常驻用户态循环程序。这些属于后续增强，不影响当前主线验收。

## 2. 文件总览

核心新增和修改文件如下：

```text
kernel/agent.h       Agent-OS ABI：常量、结构体、内核函数声明
kernel/agent.c       Agent 核心实现：Context、Tool Call、AgentFS、Agent Loop
kernel/sysagent.c    Agent-OS 系统调用入口
kernel/proc.h        PCB 扩展字段
kernel/proc.c        进程生命周期接入：初始化、释放、fork 继承
kernel/syscall.h     新增系统调用编号
kernel/syscall.c     新增系统调用分发表项
kernel/trap.c        时钟中断中调用 agent_tick()
kernel/defs.h        agent_tick() 等内核函数声明
user/user.h          用户态系统调用声明
user/usys.pl         用户态 syscall stub 生成
user/agenttest.c     任务一到任务四主测试程序
user/agentlooptest.c 任务五测试程序
Makefile             编译 agent.o、sysagent.o、agenttest、agentlooptest
```

整体调用路径：

```text
用户态 Agent 程序
  -> agent_create / tool_call / context_query / agent_wait 等 syscall stub
  -> kernel/syscall.c 按 syscall number 分发
  -> kernel/sysagent.c copyin 参数并调用 Agent 内核函数
  -> kernel/agent.c 执行 Agent 逻辑
  -> copyout 返回结构化结果，或写入用户态 Agent Context 区
```

## 3. ABI 与结构化协议

公共 ABI 定义在 `kernel/agent.h`，用户态程序可直接包含：

```c
#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"
```

关键常量：

```c
#define AGENT_CONTEXT_REGION_SIZE (8192)

#define AGENT_TYPE_NORMAL  (0)
#define AGENT_TYPE_PRIMARY (1)
#define AGENT_TYPE_WORKER  (2)

#define AGENT_LOOP_IDLE        (0)
#define AGENT_LOOP_READY       (1)
#define AGENT_LOOP_RUNNING     (2)
#define AGENT_LOOP_WAITING     (3)
#define AGENT_LOOP_ROLLED_BACK (4)
#define AGENT_LOOP_DONE        (5)

#define AGENT_EVENT_NONE      (0)
#define AGENT_EVENT_HEARTBEAT (1)
#define AGENT_EVENT_MESSAGE   (2)

#define AGENT_TOOL_OK                 (0)
#define AGENT_TOOL_ERR_TOOL_NOT_FOUND (-1)
#define AGENT_TOOL_ERR_BAD_PARAM      (-2)
#define AGENT_TOOL_ERR_NOT_AGENT      (-3)
#define AGENT_TOOL_ERR_NO_SPACE       (-4)
```

工具调用协议采用“固定结构体 + 键值对字符串”。这样避免在 xv6 内核里实现复杂 JSON parser，同时仍具备可解析、可扩展和明确错误处理能力。

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

请求示例：

```text
tool   = "query_file"
params = "type=memory;owner=Agent-B;tags=social;keyword=social"
```

响应示例：

```text
status = AGENT_TOOL_OK
result = "{status=ok,files=[{path=agentbmem,type=memory,owner=Agent-B,tags=social,summary=...}],count=1,used_index=1,index_scanned=1,full_scanned=3}"
```

参数解析函数位于 `kernel/agent.c`：

```c
static int
param_value(const char *params, const char *key, char *out, int outsz)
```

它按照 `key=value;key=value` 解析参数。工具内部如果发现缺少参数、参数格式不合法或目标不存在，会返回 `AGENT_TOOL_ERR_BAD_PARAM` 等错误码。

## 4. PCB 扩展与进程生命周期

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
uint64 heartbeat_deadline;
uint64 wakeup_tick;
uint16 context_offsets[AGENT_CONTEXT_MAX_NODES];
uint16 context_lengths[AGENT_CONTEXT_MAX_NODES];
int watch_mask;
int pending_events;
int last_wakeup_reason;
char agent_message[AGENT_MESSAGE_MAX];
```

字段职责：

```text
agent_type              普通进程或 Agent 进程
heartbeat_interval      心跳周期，单位为 tick
resource_quota          Context Path 字节配额
loop_state              Agent Loop 当前状态
context_region_start    用户态 Agent Context 区起始地址
context_region_size     Agent Context 区大小
context_path_len        当前 Context Path 已使用字节数
context_node_count      当前上下文节点数量
context_dropped_nodes   FIFO 淘汰过的节点数量
heartbeat_deadline      下一次心跳到期 tick
wakeup_tick             最近一次唤醒发生的 tick
context_offsets         每个上下文节点在 Context 区中的偏移
context_lengths         每个上下文节点长度
watch_mask              Agent 关注的事件类型位图
pending_events          尚未被 agent_wait 消费的事件
last_wakeup_reason      最近一次唤醒原因
agent_message           send_message 使用的消息槽
```

生命周期接入点在 `kernel/proc.c`：

```c
// allocproc()
p->pid = allocpid();
p->state = USED;
agent_init_proc(p);
```

释放进程时重置 Agent 字段：

```c
// freeproc()
agent_init_proc(p);
p->state = UNUSED;
```

`fork()` 时继承 Agent 元信息：

```c
np->sz = p->sz;
agent_after_fork(np, p);
```

xv6 原本会通过 `uvmcopy()` 复制父进程用户地址空间，因此 Agent Context 区内容随地址空间一起复制。`agent_after_fork()` 负责把 PCB 中的 Agent 元信息同步到子进程。

## 5. Agent 进程创建与 Context 区

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

核心逻辑在 `agent_mark_current()`：

```c
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
```

当前地址空间策略是：在用户地址空间当前末尾追加 8192 字节作为 Agent Context 区。

```text
低地址
  text / rodata
  data / bss
  heap
  Agent Context 区
  user stack
高地址
```

这块区域由用户态直接读写，适合保存高频访问的策略性数据，如上下文路径缓存、工具调用结果缓存、工具历史摘要等。内核保存可信元信息和配额，用户态保存高频数据，符合“机制与策略分离”的设计目标。

## 6. Agent Context Header

Agent Context 区开头是 header：

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

内核通过 `agent_sync_header()` 同步 header：

```c
hdr.magic = AGENT_CONTEXT_HEADER_MAGIC;
hdr.version = AGENT_CONTEXT_HEADER_VERSION;
hdr.region_size = p->context_region_size;
hdr.path_offset = sizeof(struct agent_context_header);
hdr.path_length = p->context_path_len;
hdr.node_count = p->context_node_count;
hdr.dropped_nodes = p->context_dropped_nodes;
hdr.last_timestamp = agent_now();
```

写入用户空间时使用 `copyout()`：

```c
copyout(p->pagetable, p->context_region_start, (char*)hdr, sizeof(*hdr));
```

用户态可以直接读取：

```c
struct agent_context_header *hdr = (struct agent_context_header*)ctx;
char *path = ctx + hdr->path_offset;
write(1, path, hdr->path_length);
```

## 7. Context Path 管理

Context Path 是 Agent Loop 的探索轨迹。每个节点记录一次请求和结果：

```text
{ts=...,req=query_file(...),res={status=ok,...}}
```

追加节点的核心函数是 `agent_context_push_node()`：

```c
while(p->context_node_count >= AGENT_CONTEXT_MAX_NODES ||
      p->context_path_len + rec_len > agent_path_capacity(p))
  agent_evict_oldest(p);

copyout(p->pagetable, base + p->context_path_len, record, rec_len);
p->context_offsets[p->context_node_count] = p->context_path_len;
p->context_lengths[p->context_node_count] = rec_len;
p->context_path_len += rec_len;
p->context_node_count++;
return agent_sync_header(p);
```

配额由 PCB 中的 `resource_quota` 控制：

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

超过节点数或字节配额时，内核执行 FIFO 淘汰：

```text
1. 读取最老节点长度
2. 将后续节点向前搬移
3. 更新 offset/length 数组
4. context_dropped_nodes++
5. 重新同步 header
```

相关系统调用：

```text
context_push(node)       手动追加上下文节点
context_query(buf, len)  复制 Context Path 内容到用户缓冲区
context_rollback(n)     回滚到前 n 个节点
context_clear()          清空路径和统计信息
```

另外，`tool_call()` 会自动把每次工具调用记录到 Context Path 中，因此 Agent 的多轮推理历史不需要用户态手动维护。

## 8. 系统调用列表

当前 Agent-OS 系统调用包括：

```text
agent_create(type, heartbeat_interval, quota)
agent_info(info)

tool_call(req, resp)
tool_list(buf, len)

context_push(node)
context_query(buf, len)
context_rollback(keep_nodes)
context_clear()

agent_heartbeat_set(interval)
agent_heartbeat_stop()
agent_watch(mask)
agent_watch_file(path)
agent_sched_set(priority, quota)
agent_wait(continue_loop, event)
agent_unwatch(mask)
```

系统调用编号定义在 `kernel/syscall.h`：

```c
#define SYS_agent_create 24
#define SYS_agent_info 25
#define SYS_tool_call 26
#define SYS_tool_list 27
#define SYS_context_push 28
#define SYS_context_query 29
#define SYS_context_rollback 30
#define SYS_context_clear 31
#define SYS_agent_heartbeat_set 32
#define SYS_agent_heartbeat_stop 33
#define SYS_agent_watch 34
#define SYS_agent_wait 35
#define SYS_agent_unwatch 36
#define SYS_agent_watch_file 37
#define SYS_agent_sched_set 38
```

用户态 stub 由 `user/usys.pl` 生成，函数声明在 `user/user.h`。

## 9. Tool Call 分发器

`agent_tool_call()` 是结构化交互接口的核心：

```c
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

agent_context_push_node(p, &node);
p->loop_state = AGENT_LOOP_READY;
```

当前工具集：

| 工具 | 参数 | 功能 |
| --- | --- | --- |
| `get_system_status` | 无 | 返回进程数量、Agent 数量和 tick。 |
| `query_process` | `type=agent` 可选 | 查询进程列表，可过滤 Agent。 |
| `send_message` | `target_pid`、`message` | 给目标 Agent 写消息，并触发消息事件。 |
| `read_context` | 无 | 读取当前 Agent 的最近 Context Path 内容。 |
| `set_file_attr` | `path`、`key`、`value` | 设置文件属性。 |
| `get_file_attr` | `path`、`key` | 查询文件属性。 |
| `del_file_attr` | `path`、`key` | 删除文件属性。 |
| `query_file` | `type`、`owner`、`tags`、`keyword` 等 | 按属性和摘要查询文件。 |

`tool_list()` 返回工具列表和参数概要，便于用户态 Agent 或宿主机 LLM 获得可用工具清单。

## 10. AgentFS 文件查询扩展

结果字符串通过 `buf_puts()`、`buf_putu()` 等小工具安全拼接，避免依赖完整 libc。

任务四当前版本不再把元数据放在单独的运行时 side table 里，而是把它挂在真实 inode 上，再在内存中建立查询索引。

### 10.1 真实 inode 元数据

磁盘 inode 结构 `struct dinode` 新增了 AgentFS 元数据字段：

```c
struct inode_attr {
  char key[INODE_ATTR_KEY_MAX];
  char value[INODE_ATTR_VALUE_MAX];
};

struct dinode {
  ...
  uint addrs[NDIRECT+1];
  short attr_count;
  short meta_reserved;
  char summary[INODE_SUMMARY_MAX];
  struct inode_attr attrs[INODE_ATTR_MAX];
  char meta_padding[28];
};
```

`meta_padding` 的作用是把 `dinode` 补齐到 256 字节，这样 `BSIZE=1024` 时仍然有：

```text
IPB = 1024 / 256 = 4
```

也就是说，inode 扩展后仍然保持“每个 inode block 放 4 个 inode”，不会破坏 xv6 的基本布局假设。

内存 inode `struct inode` 也同步增加：

```c
short attr_count;
char summary[INODE_SUMMARY_MAX];
struct inode_attr attrs[INODE_ATTR_MAX];
```

`ilock()` 和 `iupdate()` 负责磁盘与内存之间的同步，所以元数据会跟随 inode 一起持久化，而不是只存在于本次启动周期。

### 10.2 文件属性系统

当前提供的工具仍然是：

```text
set_file_attr(path,key,value)
get_file_attr(path,key)
del_file_attr(path,key)
query_file(...)
```

但实现方式已经变化：

1. `set_file_attr`
   - `namei(path)` 找到真实 inode
   - `ilock(ip)` 后直接修改 `ip->attrs[]`
   - 更新 `ip->attr_count`
   - 重新读取文件前 64 字节到 `ip->summary`
   - `iupdate(ip)` 持久化

2. `get_file_attr`
   - 直接从真实 inode 读取属性

3. `del_file_attr`
   - 直接在真实 inode 中删除属性并 `iupdate()`

因此这里已经满足了“把元数据挂在真实 inode 上”的要求。

### 10.3 内容摘要

内容摘要现在也属于 inode 元数据的一部分。刷新逻辑：

```c
static void
inode_summary_refresh(struct inode *ip)
{
  memset(ip->summary, 0, sizeof(ip->summary));
  n = readi(ip, 0, (uint64)ip->summary, 0, sizeof(ip->summary) - 1);
  ...
}
```

当前策略仍然是“取文件前 64 字节作为摘要”，然后在 `query_file` 中用子串匹配 `keyword`。

### 10.4 内存索引结构

为了满足“查询性能优于遍历所有文件逐一检查”，内核维护了一个独立的内存索引层。

缓存条目：

```c
struct agent_file_meta {
  int used;
  uint inum;
  char path[AGENT_FILE_PATH_MAX];
  int attr_count;
  char summary[INODE_SUMMARY_MAX];
  struct inode_attr attrs[INODE_ATTR_MAX];
};
```

倒排 posting：

```c
struct agent_file_posting {
  int used;
  int entry_idx;
  int next;
  char key[INODE_ATTR_KEY_MAX];
  char value[INODE_ATTR_VALUE_MAX];
};
```

全局索引：

```c
static struct agent_file_meta file_meta[AGENT_FILE_META_MAX];
static struct agent_file_posting file_postings[AGENT_FILE_POSTING_MAX];
static int file_index[AGENT_FILE_INDEX_BUCKETS];
```

其中：

```text
file_meta:
  缓存文件路径、attrs、summary，避免每次 query 都去逐个 ilock 文件

file_postings:
  为每个 key=value 建一个 posting 节点

file_index:
  哈希桶头，桶内是 posting 链表
```

### 10.5 索引构建

首次查询前，内核会从根目录开始递归扫描文件系统，建立缓存与 posting 索引：

```c
root = namei("/");
ilock(root);
file_index_walk(root, "");
...
file_rebuild_postings_locked();
```

`file_index_walk()` 会：

```text
1. 遍历目录项
2. 对每个普通文件读取真实 inode 元数据
3. 把 path + attrs + summary 放进 file_meta
4. 为每个属性插入 posting
```

因此查询结果里虽然会返回路径，但路径只是“索引缓存的展示信息”；真正的元数据来源仍然是 inode。

### 10.6 查询模式

`query_file()` 现在支持两种模式：

```text
默认模式:
  使用索引

mode=scan:
  强制全扫描
```

例如：

```text
query_file(type=config;owner=Agent-B;tags=plan;keyword=target)
query_file(type=config;owner=Agent-B;tags=plan;keyword=target;mode=scan)
```

索引模式的策略是：

```text
1. 取第一个属性条件，例如 type=config
2. 用 hash(type=config) 定位 posting bucket
3. 扫描候选 posting
4. 再检查剩余条件和 keyword
```

全扫描模式则直接遍历全部 `file_meta`。

### 10.7 查询结果与性能字段

`query_file()` 会返回：

```text
used_index
index_scanned
full_scanned
ticks_cost
```

含义：

```text
used_index:
  这次是否走索引

index_scanned:
  实际扫描了多少个候选条目

full_scanned:
  如果做全量遍历，当前需要检查多少个文件

ticks_cost:
  本次查询期间的 tick 差值
```

注意：`ticks_cost` 在小规模 benchmark 中可能为 0，因为 xv6 的 timer 粒度比较粗；但 `index_scanned` 与 `full_scanned` 是稳定的结构性性能数据。

### 10.8 对比数据

新增测试程序 `user/agentfsbench.c` 会创建一批带元数据的文件，然后分别执行：

```text
索引查询:
  query_file(...keyword=target)

强制全扫描:
  query_file(...keyword=target;mode=scan)
```

实测输出：

```text
agentfsbench: compare indexed(index_scanned=9, full_scanned=57, ticks_cost=0)
agentfsbench: compare fullscan(scanned=57, full_scanned=57, ticks_cost=0)
agentfsbench: batch_ticks indexed=0 fullscan=0
```

这组数据说明：

```text
索引查询只检查了 9 个候选文件
全扫描需要检查 57 个文件
候选集规模明显小于逐一检查所有文件
```

因此，虽然单次 `ticks_cost` 在这个规模下没有拉开，但结构性扫描成本已经明显优于全量遍历，满足题目要求里的“查询性能优于遍历所有文件逐一检查，并提供对比数据”。

这满足任务四“把元数据挂在真实 inode 上、提供索引查询、并且查询性能优于逐一遍历”的主要验收点。需要注意的是：真实文件元数据会随 inode 持久化，但查询索引缓存仍是运行时内存结构，重启后会在首次查询时重建。

## 11. Agent Loop 内核运行机制

任务五的核心目标是让 Agent 在内核层支持“等待事件 -> 被唤醒 -> 执行一轮 -> 再等待”的循环，而不是在用户态忙等。

### 11.1 心跳机制

用户态调用：

```c
agent_heartbeat_set(interval);
agent_heartbeat_stop();
```

设置心跳时，内核记录：

```c
p->heartbeat_interval = interval;
p->heartbeat_deadline = agent_now_safe() + interval;
```

时钟中断路径位于 `kernel/trap.c`：

```c
void
clockintr()
{
  uint now;

  acquire(&tickslock);
  ticks++;
  now = ticks;
  wakeup(&ticks);
  release(&tickslock);
  agent_tick(now);
}
```

`agent_tick(now)` 扫描进程表，找到到期 Agent 后设置 `AGENT_EVENT_HEARTBEAT`，并在目标进程睡眠于自身 channel 时切回 `RUNNABLE`：

```c
p->pending_events |= AGENT_EVENT_HEARTBEAT;
p->last_wakeup_reason = AGENT_EVENT_HEARTBEAT;
p->wakeup_tick = now;
p->heartbeat_deadline = now + p->heartbeat_interval;
if(p->state == SLEEPING && p->chan == p)
  p->state = RUNNABLE;
```

### 11.2 事件驱动触发

当前实现了两类事件源：消息事件和文件修改事件。

消息事件的注册方式：

```c
agent_watch(AGENT_WATCH_MESSAGE);
```

`send_message` 工具会写入目标 Agent 的消息槽，并在目标关注消息事件时设置 pending event：

```c
safestrcpy(target->agent_message, message, sizeof(target->agent_message));
if(target->watch_mask & AGENT_WATCH_MESSAGE){
  target->pending_events |= AGENT_EVENT_MESSAGE;
  target->last_wakeup_reason = AGENT_EVENT_MESSAGE;
  target->wakeup_tick = agent_now_safe();
  if(target->state == SLEEPING && target->chan == target)
    target->state = RUNNABLE;
}
```

这样消息既是结构化工具调用，也是 Agent Loop 的事件源。

文件修改事件使用专用 syscall 注册：

```c
agent_watch_file("watchlog");
```

内核在注册时会解析路径、定位真实 inode，并把 `dev + inum + path` 保存到 PCB。之后普通 `write()` 成功写入该 inode 时，`kernel/file.c` 会调用：

```c
agent_notify_file_modified(dev, inum);
```

该函数扫描进程表，找到关注该 inode 的 Agent，设置 `AGENT_EVENT_FILEMOD`，并在目标休眠于 `agent_wait()` 时立即唤醒。

### 11.3 `agent_wait()` 与生命周期

`agent_wait(continue_loop, event)` 同时负责等待和生命周期声明。

```text
continue_loop = 1:
  本轮结束，还要继续下一轮。若没有 pending event，进入 SLEEPING。

continue_loop = 0:
  任务完成。内核将 loop_state 设置为 AGENT_LOOP_DONE，并清理心跳、watch 和 pending event。
```

等待逻辑：

```c
p->loop_state = AGENT_LOOP_WAITING;
for(;;){
  reason = p->pending_events;
  if(reason != AGENT_EVENT_NONE){
    event.reason = reason;
    event.tick = p->wakeup_tick ? p->wakeup_tick : agent_now_safe();
    if(reason & AGENT_EVENT_MESSAGE)
      safestrcpy(event.message, p->agent_message, sizeof(event.message));
    p->pending_events = 0;
    p->loop_state = AGENT_LOOP_READY;
    ...
    return reason;
  }
  p->chan = p;
  p->state = SLEEPING;
  sched();
  p->chan = 0;
}
```

用户态收到的事件结构：

```c
struct agent_wait_event {
  int reason;
  uint32 reserved;
  uint64 tick;
  char message[AGENT_MESSAGE_MAX];
  char file[AGENT_MESSAGE_MAX];
};
```

因此用户态可以通过：

```c
if(reason & AGENT_EVENT_FILEMOD)
  printf("file modified: %s\n", event.file);
```

区分消息事件与文件修改事件。

### 11.4 多 Agent 协调与优先级/配额调度

当前版本在 xv6 原始调度器基础上加入了一层 Agent-aware 调度策略。每个 Agent 新增如下调度字段：

```text
agent_sched_priority   静态优先级，范围 1..8
agent_sched_quota      每轮预算可运行的调度片数
agent_sched_budget     当前轮剩余预算
agent_sched_boost      事件唤醒后的临时加权
```

默认策略：

```text
PRIMARY Agent: priority=5 quota=4
WORKER  Agent: priority=3 quota=2
普通进程:        走默认分支，不受 Agent quota 限制
```

用户态可动态调整：

```c
agent_sched_set(7, 6);
```

调度器的核心策略是：

```text
1. 在 RUNNABLE 进程中优先选择 score 更高的 Agent
2. score = agent_sched_priority + agent_sched_boost
3. Agent 每运行一个调度片，agent_sched_budget--
4. 当所有 runnable Agent 的 budget 都耗尽时，统一 refill 到各自 quota
5. 被心跳、消息或文件事件唤醒的 Agent 会获得短时 boost，保证事件响应优先
```

这样可以同时满足三件事：

```text
事件驱动 Agent 被及时响应
高优先级 Agent 比低优先级 Agent 更容易获得 CPU
quota 防止单个高优先级 Agent 长时间垄断处理器
```

## 12. 测试程序

### 12.1 `agenttest`

`user/agenttest.c` 覆盖任务一到任务四：

```text
普通进程 tool_call 被拒绝
agent_create / agent_info
Agent Context header 可读写
tool_list
get_system_status
query_process
missing_tool 错误处理
set_file_attr / get_file_attr
query_file 属性和内容摘要查询
tool_call 自动记录 Context Path
context_query
context_rollback
context_clear
小配额下 FIFO 淘汰
```

期望输出：

```text
agenttest: all tests passed
```

### 12.2 `agentlooptest`

`user/agentlooptest.c` 覆盖任务五：

```text
heartbeat_test:
  设置心跳，agent_wait 被心跳唤醒

message_only_test:
  关闭心跳，只靠 send_message 事件唤醒

worker_loop:
  子 Agent 先等心跳，再等消息，最后 agent_wait(0, 0) 标记完成

multi_agent_test:
  两个 Worker Agent 并发等待和唤醒

file_modify_event_test:
  watch_file -> 子进程 write -> Agent 被文件修改事件唤醒

scheduler_policy_test:
  高优先级/高配额 Agent 与低优先级/低配额 Agent 并发运行，对比 CPU 获得量
```

期望输出：

```text
agentlooptest: all tests passed
```

### 13.5 agentfsbench

`user/agentfsbench.c` 是任务四的性能对比测试程序。

它会：

```text
1. 创建一批测试文件
2. 为这些文件设置真实 inode 元数据
3. 运行一次索引查询
4. 运行一次强制全扫描查询
5. 输出 index_scanned / full_scanned / ticks_cost
```

主要校验点：

```text
索引查询成功
全扫描查询成功
index_scanned < full_scanned
mode=scan 时 scanned == full_scanned
```

实测输出：

```text
agentfsbench: compare indexed(index_scanned=9, full_scanned=57, ticks_cost=0)
agentfsbench: compare fullscan(scanned=57, full_scanned=57, ticks_cost=0)
agentfsbench: batch_ticks indexed=0 fullscan=0
agentfsbench: all tests passed
```

### 13.6 原有回归

建议同时运行：

```text
usertests
mmaptest
```

用于确认 Agent-OS 扩展没有破坏 xv6 原有行为和 mmap 实验。

## 14. 构建与运行

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
agentfsbench
agentlooptest
usertests
mmaptest
```

退出 QEMU：

```text
Ctrl-A 然后按 X
```

## 15. 当前实现边界

当前实现偏向教学操作系统中的可演示闭环，边界如下：

```text
Context 区由 uvmalloc 追加到用户地址空间末尾，不是独立 VMA
真实 inode 中持久化的是 attrs 和 summary，查询 posting/index 缓存仍是运行时内存结构
内容摘要是前 64 字节子串匹配，不是 embedding 语义检索
query_file 的索引策略使用“第一个属性条件 -> posting bucket -> 剩余条件过滤”，还不是多条件最优执行计划
文件事件当前只支持单文件 inode watch，还没有扩展到目录递归或通配条件
send_message 仍是单消息槽，不是完整消息队列
调度器当前采用单机内核中的简单优先级+预算轮转，还不是多核下的复杂全局公平调度
真实 LLM 演示还需要单独的 agent_loop 用户态程序或宿主机桥接脚本
```

后续增强方向可以是：Agent Context 专用 VMA、`.agentmeta` 持久化、多条件索引选择、多文件/目录 watch、消息队列、更细粒度的 Agent 调度统计，以及完整的 `agent_loop` 用户态桥接程序。
