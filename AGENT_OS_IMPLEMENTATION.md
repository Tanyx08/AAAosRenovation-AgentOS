# Agent-OS 实现说明

本文档是当前仓库中 Agent-OS 扩展的唯一权威实现说明，面向项目开发者、评审和答辩检查人员。它覆盖赛题任务一到任务五，以及本项目额外实现的三个创新点：

```text
创新一：跨 Agent 共享 query_file 查询结果
创新二：事件感知调度
创新三：动态工具注册，类似 LLM skill/tool 的按需扩展
```

演示和使用步骤请看 `AGENT_OS_INTERFACE_LLM_GUIDE.md`。创新想法来源和落地状态请看 `AGENT_IDEAS.md`。

## 1. 系统目标

原始 xv6 只把进程视为普通用户程序。Agent-OS 扩展的目标是把 xv6 改造成能识别、管理和调度 Agent 的教学操作系统原型：

```text
让 xv6 能识别 Agent 进程
让 Agent 通过结构化 Tool Call 与内核交互
让 Agent 拥有可追踪的 Context Path
让 Agent 能按文件属性和摘要查询 AgentFS 元数据
让 Agent 能等待心跳、消息和文件修改事件
让调度器理解事件紧急程度
让用户态服务能动态注册工具能力
```

当前实现基于 `xv6-2023-mit-labs` 的 `mmap` 分支，新增内核 Agent 元数据、用户态 Agent Context 区、结构化工具协议、AgentFS 运行时元数据层、共享查询缓存、Agent Loop、事件感知调度和动态工具注册表。

## 2. 完成度总览

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| 任务一：Agent 进程创建与地址空间设计 | 已完成 | `agent_create`、`agent_info`、PCB 扩展、用户态 Agent Context 区已实现。 |
| 任务二：结构化交互接口 | 已完成 + 增强 | `tool_call`、`tool_list` 和 8 个内置工具已实现，并支持动态注册工具。 |
| 任务三：Context Path 管理 | 已完成 | 上下文节点追加、查询、回滚、清空、配额和 FIFO 淘汰已实现。 |
| 任务四：Agent 查询优化文件系统扩展 | 已完成 + 增强 | 文件属性、摘要、哈希索引、扫描统计和共享查询缓存已实现。 |
| 任务五：Agent Loop 内核运行机制 | 已完成 + 增强 | 心跳、消息、FILEMOD、`agent_wait` 生命周期和事件感知调度已实现。 |
| 创新一：共享查询缓存 | 已完成 | 多 Agent 查询相同 public/system/同组条件时可复用 `query_file` 结果。 |
| 创新二：事件感知调度 | 已完成 | 调度分数综合 Agent priority、事件权重和 aging。 |
| 创新三：动态工具注册 | 已完成 | 用户态 Agent 服务可注册工具，调用方通过统一 `tool_call` 访问。 |

## 3. 总体架构

整体结构分为用户态 Agent、系统调用入口、Agent 内核核心层、Context 层、AgentFS 层、Agent Loop 层、工具调用层和调度层：

```text
User Agent / Tool Service
  |
  | syscall stubs: user/usys.pl, user/user.h
  v
kernel/syscall.c
  |
  v
kernel/sysagent.c
  |  参数 copyin/copyout，调用内核 Agent API
  v
kernel/agent.c
  |-- Agent process metadata
  |-- agent_create / agent_info / fork 继承默认值
  |
  |--> kernel/agent_context.c
  |     |-- Agent Context 区
  |     |-- Context Path 追加 / 查询 / 回滚 / 清空 / 淘汰
  |
  |--> kernel/agent_fs.c
  |     |-- AgentFS metadata / index / cache
  |     |-- set/get/del/query_file
  |
  |--> kernel/agent_loop.c
  |     |-- heartbeat / message / filemod
  |     |-- agent_wait / lifecycle / scheduler score
  |
  |--> kernel/agent_tool.c
  |     |-- Built-in Tool Call
  |     |-- Dynamic tool registry / request table
  |
  v
kernel/proc.c
  |-- fork/exit 生命周期接入
  |-- event-aware scheduler
```

这个分层有三个好处：

```text
sysagent.c 只负责 syscall 边界和用户/内核拷贝
agent.c 只保留 Agent 核心元信息与创建逻辑
agent_context/agent_fs/agent_loop/agent_tool 四个文件分别对应任务三/四/五/二
proc.c 只在进程生命周期和调度点接入 Agent 逻辑
```

## 4. 代码文件与模块边界

核心文件职责如下：

```text
kernel/agent.h
  Agent-OS 公共 ABI、常量、结构体和内核函数声明。

kernel/agent.c
  Agent 核心逻辑：进程标记、默认调度参数、PCB 中 Agent 字段初始化、
  agent_create / agent_info / fork 继承。

kernel/agent_context.c
  用户态 Agent Context 区与 Context Path 管理：
  Header 同步、路径追加、查询、回滚、清空和 FIFO 淘汰。

kernel/agent_fs.c
  任务四 AgentFS：
  inode 属性维护、摘要刷新、运行时元数据缓存、倒排索引、
  query_file 查询优化和共享查询缓存。

kernel/agent_loop.c
  任务五 Agent Loop：
  心跳、消息事件、文件修改事件、agent_wait 生命周期、
  调度评分、tick 驱动唤醒。

kernel/agent_tool.c
  任务二和动态工具机制：
  内置工具分发、tool_list、动态工具注册/收取/回复、
  动态工具请求表与权限控制。

kernel/sysagent.c
  Agent 系统调用入口，负责 argint/argaddr/argstr、copyin/copyout。

kernel/proc.h
  在 struct proc 中加入 Agent 元数据、事件状态、调度字段。

kernel/proc.c
  allocproc/freeproc/fork/exit 接入 Agent 初始化、继承和清理；
  scheduler 接入事件感知评分。

kernel/trap.c
  时钟中断中调用 agent_tick(now)，触发心跳事件。

kernel/syscall.h, kernel/syscall.c
  Agent syscall 编号和分发表项。

user/user.h, user/usys.pl
  用户态 syscall 声明和 stub 生成。

user/agenttest.c
  任务一到任务四基础测试。

user/agentlooptest.c
  Agent Loop、心跳、消息事件和多 Agent 测试。

user/agentinnovationtest.c
  三个创新点的集中演示测试。

Makefile
  编译 agent.o、agent_context.o、agent_fs.o、agent_loop.o、
  agent_tool.o、sysagent.o 和测试程序。
```

其中当前 Agent 内核模块和赛题任务的对应关系可以直接概括为：

```text
任务一:
  kernel/agent.c + kernel/agent_context.c

任务二:
  kernel/agent_tool.c

任务三:
  kernel/agent_context.c

任务四:
  kernel/agent_fs.c

任务五:
  kernel/agent_loop.c + kernel/proc.c + kernel/trap.c
```

## 5. Agent ABI 与系统调用

公共 ABI 位于 `kernel/agent.h`。用户程序通常包含：

```c
#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"
```

系统调用按功能分组：

```text
Agent 创建与查询:
  agent_create(type, heartbeat_interval, quota)
  agent_info(info)

结构化工具:
  tool_call(req, resp)
  tool_list(buf, len)

Context Path:
  context_push(node)
  context_query(buf, len)
  context_rollback(keep_nodes)
  context_clear()

Agent Loop:
  agent_heartbeat_set(interval)
  agent_heartbeat_stop()
  agent_watch(mask)
  agent_watch_file(path)
  agent_wait(continue_loop, event)
  agent_unwatch(mask)
  agent_priority_set(priority)
  agent_sched_set(priority, quota)

动态工具:
  tool_register(name, flags)
  tool_recv(request)
  tool_reply(request_id, result, status)
```

关键常量：

```c
#define AGENT_TYPE_NORMAL  (0)
#define AGENT_TYPE_PRIMARY (1)
#define AGENT_TYPE_WORKER  (2)

#define AGENT_EVENT_HEARTBEAT (1)
#define AGENT_EVENT_MESSAGE   (2)
#define AGENT_EVENT_FILEMOD   (4)

#define AGENT_WATCH_MESSAGE AGENT_EVENT_MESSAGE
#define AGENT_WATCH_FILEMOD AGENT_EVENT_FILEMOD

#define AGENT_TOOL_FLAG_PUBLIC (1)
```

结构化工具协议使用固定结构体和 `key=value;key=value` 参数字符串。这样避免在 xv6 内核里实现复杂 JSON parser，同时保留清晰的字段边界和错误码。

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

## 6. Agent 进程模型

Agent 元数据保存在 `struct proc` 中。关键字段包括：

```text
agent_type              普通进程、Primary Agent 或 Worker Agent
agent_group             Agent 协作组，用于共享缓存和动态工具权限
agent_priority          事件感知调度的基础优先级
resource_quota          Context Path 字节配额
loop_state              Agent Loop 生命周期状态
context_region_start    用户态 Agent Context 区起始地址
context_region_size     Agent Context 区大小
heartbeat_deadline      下一次心跳到期 tick
watch_mask              Agent 关注的事件类型
pending_events          已到达但尚未被 agent_wait 消费的事件
agent_message           send_message 使用的单消息槽
```

生命周期接入点：

```text
allocproc:
  agent_init_proc(p) 初始化 Agent 字段

agent_create:
  agent_mark_current() 把当前进程标记为 Agent
  分配用户态 Context 区
  初始化 loop_state、heartbeat、group、priority

fork:
  agent_after_fork(np, p) 继承 Agent 元信息
  Worker 默认继承父进程 Agent group

exit:
  agent_proc_exit(p) 清理动态工具注册项和挂起请求

freeproc:
  agent_init_proc(p) 重置字段
```

Primary Agent 调用 `agent_create(AGENT_TYPE_PRIMARY, ...)` 时会以自身 pid 建立新的 `agent_group`。Worker Agent 从父进程继承 group，因此同一组 Agent 可以共享查询缓存和默认访问同组动态工具。

## 7. Context Path 上下文管理

Context Path 对应任务三，是 Agent 的工具调用历史和上下文轨迹。它不是普通日志，而是供用户态 Agent 或宿主机 LLM 快速读取的结构化上下文。

用户态 Context 区由 `agent_create()` 追加到进程地址空间末尾：

```text
context_region_start
  |
  | struct agent_context_header
  |
  | node record 0
  | node record 1
  | ...
```

Header 记录 magic、版本、区域大小、路径长度、节点数量、淘汰数量和最近结果。节点由 `struct agent_context_node` 表示：

```c
struct agent_context_node {
  uint64 timestamp_ms;
  char request[AGENT_CONTEXT_REQ_MAX];
  char result[AGENT_CONTEXT_RES_MAX];
};
```

核心操作：

```text
context_push:
  追加用户显式提供的上下文节点

tool_call:
  每次工具调用后自动追加 request/result 节点

context_query:
  从用户态 Context 区复制当前 Context Path

context_rollback:
  保留前 N 个节点，丢弃后续节点

context_clear:
  清空路径和 header 统计

quota eviction:
  超过 resource_quota 或 AGENT_CONTEXT_MAX_NODES 时 FIFO 淘汰最旧节点
```

这个设计使 Agent 可以在多轮工具调用后回看“自己为什么走到当前状态”，也便于 LLM 桥接程序把最近上下文提供给模型。

## 8. Tool Call 结构化交互

Tool Call 对应任务二。普通进程不能直接调用 Agent 工具；必须先通过 `agent_create()` 成为 Agent。当前这部分实现主要位于 `kernel/agent_tool.c`，内核在 `agent_tool_call()` 中进行检查：

```text
普通进程:
  返回 AGENT_TOOL_ERR_NOT_AGENT

Agent 进程:
  设置 loop_state = AGENT_LOOP_RUNNING
  调用内置工具或动态工具
  自动写入 Context Path
  设置 loop_state = AGENT_LOOP_READY
```

内置工具：

| 工具 | 参数 | 功能 |
| --- | --- | --- |
| `get_system_status` | 无 | 返回进程数量、Agent 数量和 tick。 |
| `query_process` | `type=agent` 可选 | 查询进程列表，可过滤 Agent。 |
| `send_message` | `target_pid`、`message` | 写入目标 Agent 消息槽并触发 MESSAGE 事件。 |
| `read_context` | 无 | 读取当前 Agent 最近 Context Path 内容。 |
| `set_file_attr` | `path`、`key`、`value` | 设置 AgentFS 文件属性。 |
| `get_file_attr` | `path`、`key` | 查询文件属性。 |
| `del_file_attr` | `path`、`key` | 删除文件属性。 |
| `query_file` | `type`、`owner`、`tags`、`keyword`、`public` 等 | 按属性和摘要查询文件。 |

`tool_list()` 会返回内置工具列表，并附加当前调用方可见的动态工具，例如：

```text
...;query_file(type,owner,tags,keyword,public);summarize_log(dynamic)
```

## 9. AgentFS 文件查询系统

AgentFS 对应任务四。当前这部分实现集中位于 `kernel/agent_fs.c`。文件属性和摘要进入 xv6 inode/dinode：`set_file_attr` 会更新 inode 中的 attrs/summary 并 `iupdate()` 写回磁盘；内核同时维护运行时索引缓存，用于加速 `query_file`。

核心结构：

```c
struct agent_file_meta {
  int used;
  uint inum;
  char path[AGENT_FILE_PATH_MAX];
  char summary[INODE_SUMMARY_MAX];
  int attr_count;
  struct inode_attr attrs[INODE_ATTR_MAX];
};
```

主要流程：

```text
set_file_attr:
  namei(path) 校验普通文件存在
  更新 inode key/value 属性
  刷新 summary
  iupdate 写回 dinode
  重建运行时属性哈希索引
  递增 AgentFS version
  触发 FILEMOD 事件

get_file_attr:
  从 inode 读取指定属性

del_file_attr:
  删除 inode 属性
  iupdate 写回 dinode
  重建索引
  递增版本
  触发 FILEMOD 事件

query_file:
  解析属性条件和 keyword
  优先使用第一个属性条件定位哈希桶
  再检查剩余属性和 summary 子串
  返回结构化结果和扫描统计
```

`query_file` 返回字段中，和查询优化相关的是：

```text
used_index      是否使用属性哈希索引
index_scanned   实际扫描的索引项数量
full_scanned    当前 AgentFS 元数据表中已使用项数量
```

示例：

```text
{status=ok,files=[{path=agentbmem,type=memory,owner=Agent-B,tags=social,summary=...}],count=1,used_index=1,index_scanned=1,full_scanned=3,cache_hit=0,cache_owner=3,cache_refcnt=1,cache_version=4,fs_scanned=3}
```

## 10. 创新一：跨 Agent 共享查询缓存

### 10.1 设计动机

多个 Agent 经常会查询相同条件，例如：

```text
query_file(type=memory;owner=system;tags=shared;public=true)
```

如果每个 Agent 都重新扫描 AgentFS 元数据，会造成重复工作。共享查询缓存让第一个 Agent 的查询结果可以被其他满足安全条件的 Agent 复用。

### 10.2 数据结构

```c
struct shared_query_cache {
  int used;
  char query[AGENT_TOOL_PARAM_MAX];
  char result[AGENT_TOOL_RESULT_MAX];
  int owner_pid;
  int owner_group;
  int refcnt;
  uint64 version;
};
```

### 10.3 命中和写入流程

```text
query_file 调用
  |
  v
shared_query_cache_lookup
  |
  |-- 命中:
  |     版本一致
  |     查询字符串一致
  |     访问权限通过
  |     返回 cache_hit=1, fs_scanned=0
  |
  |-- 未命中:
        执行原 AgentFS 查询
        shared_query_cache_store
        返回 cache_hit=0
```

### 10.4 安全共享规则

缓存不是对所有 Agent 无条件开放。满足任一条件才允许共享：

```text
查询参数包含 public=true
查询条件包含 owner=system
调用者与缓存 owner 属于同一个 agent_group
```

这样可以演示“跨 Agent 共享结果”和“安全隔离”同时存在。

### 10.5 版本失效

`set_file_attr` 和 `del_file_attr` 会调用 `agent_file_version_bump()` 递增全局版本。缓存项记录创建时的 version；查询时如果版本不一致，就视为未命中。

演示关注字段：

```text
cache_hit=0      第一次真实查询
cache_hit=1      后续命中共享缓存
fs_scanned=0     命中缓存后没有真实 AgentFS 扫描
cache_refcnt     缓存被复用次数
cache_version    缓存对应的 AgentFS 元数据版本
```

## 11. Agent Loop：事件等待与生命周期

Agent Loop 对应任务五。当前这部分实现集中位于 `kernel/agent_loop.c`，核心目标是让 Agent 在无事可做时真正睡眠，并由内核事件唤醒，而不是在用户态忙等。

### 11.1 事件类型

当前支持三类事件：

```text
AGENT_EVENT_HEARTBEAT
  周期性心跳事件，由时钟中断触发。

AGENT_EVENT_MESSAGE
  消息事件，由 send_message 工具触发。

AGENT_EVENT_FILEMOD
  文件元数据修改事件，由 set_file_attr / del_file_attr 触发。
```

用户态通过 `agent_watch(mask)` 注册关注事件：

```c
agent_watch(AGENT_WATCH_MESSAGE);
agent_watch(AGENT_WATCH_FILEMOD);
```

### 11.2 心跳机制

用户态调用：

```c
agent_heartbeat_set(interval);
agent_heartbeat_stop();
```

设置心跳时内核记录：

```text
heartbeat_interval
heartbeat_deadline
```

时钟中断路径：

```text
clockintr()
  ticks++
  agent_tick(now)
```

`agent_tick(now)` 扫描进程表，找到到期 Agent 后：

```text
pending_events |= AGENT_EVENT_HEARTBEAT
wakeup_tick = now
heartbeat_deadline = now + interval
如果进程睡眠在 agent_wait channel，则切换为 RUNNABLE
```

### 11.3 消息事件

`send_message` 工具会把消息写入目标 Agent 的 `agent_message`。如果目标 Agent 关注了 `AGENT_WATCH_MESSAGE`，内核会设置 MESSAGE pending event 并唤醒目标。

这条路径同时服务两个目的：

```text
结构化工具调用：send_message(target_pid,message)
Agent Loop 事件源：AGENT_EVENT_MESSAGE
```

### 11.4 文件修改事件

AgentFS 元数据变化也可以成为事件源。`set_file_attr` 和 `del_file_attr` 修改运行时文件属性后，会唤醒关注 `AGENT_WATCH_FILEMOD` 的 Agent。

这使 Agent 可以写出“文件/记忆变化后再行动”的循环，而不是只能依赖心跳或消息。

### 11.5 agent_wait 生命周期语义

用户态通过 `agent_wait()` 声明本轮 Agent Loop 的状态：

```text
agent_wait(1, &event)
  本轮结束，继续下一轮。
  如果没有 pending event，则进入 SLEEPING。
  被事件唤醒后返回 reason，并把事件信息复制到用户态。

agent_wait(0, 0)
  任务完成。
  内核设置 loop_state = AGENT_LOOP_DONE。
  清理 heartbeat、watch_mask、pending_events 和消息槽。
```

返回事件结构：

```c
struct agent_wait_event {
  int reason;
  uint32 reserved;
  uint64 tick;
  char message[AGENT_MESSAGE_MAX];
};
```

`loop_state` 主要流转：

```text
AGENT_LOOP_READY
  -> AGENT_LOOP_RUNNING   tool_call 执行中
  -> AGENT_LOOP_WAITING   agent_wait(1) 且无事件
  -> AGENT_LOOP_READY     被事件唤醒
  -> AGENT_LOOP_DONE      agent_wait(0)
```

## 12. 创新二：事件感知调度

### 12.1 设计动机

普通优先级是静态的；Agent 的紧急程度却经常由事件决定。同一个 Agent 收到用户消息时应该更快运行，而普通心跳不应过度抢占 CPU。

因此调度器不只看 `agent_priority`，还看 `pending_events`。

### 12.2 调度分数

每轮 scheduler 为 RUNNABLE 进程计算：

```text
score = agent_priority * 10 + event_weight(pending_events) + aging_bonus
```

事件权重：

```text
MESSAGE   30
FILEMOD   20
HEARTBEAT 10
NONE       0
```

默认 `agent_priority` 是 5，可通过：

```c
agent_priority_set(priority);
```

调整。

### 12.3 scheduler 改动

原始 xv6 scheduler 按进程表顺序运行遇到的 RUNNABLE 进程。当前实现改为：

```text
扫描 proc table
  对每个 RUNNABLE 进程计算 score
  选择最高分进程
  swtch 到该进程
```

普通进程也有默认基础分和 aging。aging 会随等待时间增长，避免普通进程或低事件权重 Agent 长期饥饿。

### 12.4 演示语义

`agentinnovationtest` 创建三个 Worker：

```text
Agent-M: pending MESSAGE
Agent-F: pending FILEMOD
Agent-H: pending HEARTBEAT
```

在同优先级下，预期运行顺序是：

```text
MESSAGE -> FILEMOD -> HEARTBEAT
```

在 SMP QEMU 上，多个 hart 可能并行执行；测试中用很小的用户态输出延迟稳定展示顺序，内核评分逻辑仍由 `agent_schedule_score()` 决定。

## 13. 创新三：动态工具注册

### 13.1 设计动机

内核固定工具适合基础能力，但 LLM Agent 的真实工作方式更像 skill：某个服务按需声明能力，其他 Agent 通过统一工具接口调用它。动态工具注册实现了这个模型。

### 13.2 接口和数据结构

用户态接口：

```c
int tool_register(const char *name, int flags);
int tool_recv(void *request);
int tool_reply(int request_id, const char *result, int status);
```

内核维护两个固定小表：

```text
dynamic_tools:
  工具名、owner pid、owner group、flags

dynamic_requests:
  request id、caller pid、service pid、tool、params、result、status
```

固定小表符合 xv6 教学内核风格，避免引入复杂动态内存和通用 IPC 子系统。

### 13.3 调用流程

完整流程：

```text
1. 服务 Agent:
   tool_register("summarize_log", AGENT_TOOL_FLAG_PUBLIC)

2. 调用方 Agent:
   tool_call("summarize_log", "file=agentlog")

3. 内核:
   内置工具表未命中
   查找 dynamic_tools
   创建 dynamic_request
   唤醒服务进程
   调用方睡眠等待 reply

4. 服务 Agent:
   tool_recv(&req)
   读取 req.tool / req.params / req.caller_pid
   执行业务逻辑

5. 服务 Agent:
   tool_reply(req.request_id, "{summary=...}", AGENT_TOOL_OK)

6. 内核:
   写回响应
   唤醒调用方
   tool_call 返回结果
```

### 13.4 权限和退出清理

权限规则：

```text
默认只允许同 agent_group 调用
AGENT_TOOL_FLAG_PUBLIC 允许跨 group 调用
普通进程不能注册动态工具
工具名不能与内置工具冲突
工具名重复注册会被拒绝
```

服务进程退出时，`agent_proc_exit()` 会：

```text
清理该进程注册的 dynamic_tools
把等待该服务的请求标记为 AGENT_TOOL_ERR_SERVICE_GONE
唤醒等待中的调用方
```

当前没有实现复杂超时和取消策略，这是后续工作。

## 14. 并发、安全与资源控制

关键同步和安全点集中如下：

```text
agent_file_lock
  保护 AgentFS 元数据表和属性索引。

agent_runtime_lock
  保护共享查询缓存、动态工具表和动态请求表。

p->lock
  保护单个进程的 state、pending_events、watch_mask、消息槽和调度字段。

普通进程隔离
  未调用 agent_create 的普通进程不能调用 Agent 工具或注册动态工具。

共享查询缓存隔离
  只有 public=true、owner=system 或同 agent_group 查询可共享。

动态工具权限
  默认同组可调用，AGENT_TOOL_FLAG_PUBLIC 才允许跨组。

Context Path 资源控制
  resource_quota 限制上下文字节数，超限 FIFO 淘汰旧节点。

调度防饥饿
  aging_bonus 随等待时间增加，避免低权重事件或普通进程长期得不到 CPU。
```

此外，`sys_tool_call()` 中的大请求/响应结构体使用 `kalloc()` 分配，避免深层工具调用链占用过多内核栈。

## 15. 测试与验证

构建：

```bash
make
make fs.img
```

启动 xv6：

```bash
make qemu
```

在 xv6 shell 中运行：

```text
agenttest
agentlooptest
agentfsbench
agentperftest
agentinnovationtest
```

期望结果：

```text
agenttest: all tests passed
agentlooptest: all tests passed
agentfsbench: all tests passed
agentperftest: all tests passed
agentinnovationtest: all tests passed
```

测试覆盖：

```text
agenttest:
  普通进程 tool_call 拒绝
  agent_create / agent_info
  Context header
  tool_list
  get_system_status / query_process
  set_file_attr / get_file_attr / query_file
  Context Path 自动记录、查询、回滚、清空、配额淘汰

agentlooptest:
  heartbeat_test
  message_only_test
  filemod/watch_file 测试
  worker_loop 生命周期
  multi_agent_test 多 Agent 等待和唤醒
  priority/quota 调度测试

agentfsbench:
  批量创建带属性文件
  对比索引查询和 mode=scan 全表扫描
  验证 index_scanned 小于 full_scanned

agentperftest:
  query_file 索引查询 vs mode=scan 全表扫描
  HEARTBEAT / MESSAGE / FILEMOD 唤醒延迟
  agent_wait 空闲休眠 vs 用户态轮询空转
  高低 priority/quota Worker 调度效果
  详细方案见 AGENT_PERFORMANCE_TESTS.md

agentinnovationtest:
  shared_cache_test
    cache_hit=0 -> cache_hit=1 -> 元数据修改后 cache_hit=0

  event_scheduler_test
    MESSAGE -> FILEMOD -> HEARTBEAT

  dynamic_tool_test
    summarize_log 注册、接收请求、回复结果
```

建议保留原 xv6/mmap 回归：

```text
usertests
mmaptest
```

## 16. 当前边界与后续工作

当前实现偏向教学操作系统中的可演示闭环，边界如下：

```text
Agent Context 区使用 uvmalloc 追加到用户地址空间末尾，不是独立 VMA
query_file 的内容检索是摘要子串匹配，不是 embedding 或语义搜索
query_file 的索引策略只使用第一个属性条件定位哈希桶
send_message 使用单消息槽，不是完整消息队列
动态工具请求表是固定小表
动态工具没有复杂超时、取消和重试策略
真实 LLM 推荐运行在宿主机，通过串口桥接到 xv6 用户态 Agent
```

自然的后续方向：

```text
Agent Context 专用 VMA
多条件索引选择
消息队列
动态工具超时/取消机制
更细粒度的工具权限策略
完整 agent_loop 用户态桥接程序
```
