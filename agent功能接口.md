# Agent 内核模块总览

本文档根据当前仓库的实际代码结构，说明 Agent-OS 内核侧几个核心文件分别负责什么，方便阅读源码、答辩讲解和后续继续维护。

当前 Agent 内核实现已经拆分为 5 个职责清晰的模块：

```text
kernel/agent.c
kernel/agent_context.c
kernel/agent_fs.c
kernel/agent_loop.c
kernel/agent_tool.c
```

另外，系统调用入口位于：

```text
kernel/sysagent.c
```

## 1. 总体分层

当前内核侧调用链可以概括为：

```text
user program
  |
  | user/user.h + usys.pl
  v
kernel/sysagent.c
  |
  v
kernel/agent*.c
  |
  +-- agent.c         Agent 核心元信息与创建逻辑
  +-- agent_context.c Context 区和 Context Path
  +-- agent_fs.c      AgentFS 与 query_file
  +-- agent_loop.c    心跳、事件、等待、调度评分
  +-- agent_tool.c    工具调用与动态工具机制
  |
  v
kernel/proc.c / trap.c / file.c
```

这样拆分后，赛题任务和代码文件的关系也比较直观：

```text
任务一：agent.c + agent_context.c
任务二：agent_tool.c
任务三：agent_context.c
任务四：agent_fs.c
任务五：agent_loop.c
```

## 2. 各文件职责与主要对外接口

### `kernel/agent.c`

这是 Agent 的“核心元信息模块”，主要负责：

- `agent_init_proc()`：初始化 `struct proc` 中的 Agent 扩展字段
- `agent_after_fork()`：在 `fork` 后继承 Agent 元信息
- `agent_mark_current()`：把当前进程标记为 Agent，并分配用户态 Context 区
- `agent_get_info()`：把当前 Agent 元信息导出给用户态
- Agent 默认优先级、默认配额和基础时间读取辅助

对外提供给其他内核模块使用的接口主要有：

- `agent_init_proc()`
- `agent_after_fork()`
- `agent_mark_current()`
- `agent_get_info()`
- `agent_now_ticks()`
- `agent_default_priority()`
- `agent_default_quota()`

这个文件现在不再负责：

- Context Path 细节
- AgentFS
- Agent Loop
- Tool Call 分发

所以它已经接近“Agent core”。

### `kernel/agent_context.c`

这是任务一和任务三最直接的实现文件，负责用户态 Agent Context 区和 Context Path 管理：

- `agent_path_capacity()`
- `agent_path_base()`
- `agent_context_write_header()`
- `agent_evict_oldest()`
- `agent_sync_header()`
- `agent_context_clear()`
- `agent_context_push_node()`
- `agent_context_query()`
- `agent_context_rollback()`

对外接口主要有：

- `agent_context_clear()`
- `agent_context_push_node()`
- `agent_context_query()`
- `agent_context_rollback()`

这个文件主要回答 4 个问题：

- Context 区在用户地址空间哪里
- Header 怎么同步
- 路径节点怎么追加
- 超配额时怎么 FIFO 淘汰、怎么回滚和清空

### `kernel/agent_fs.c`

这是任务四的独立模块，负责 AgentFS 文件系统扩展和查询优化：

- 真实 inode 上的属性与摘要维护
- 文件元数据缓存 `file_meta[]`
- 倒排索引 `file_postings[] + file_index[]`
- 共享查询缓存 `shared_query_cache[]`
- 文件工具：
  - `agentfs_tool_set_file_attr()`
  - `agentfs_tool_get_file_attr()`
  - `agentfs_tool_del_file_attr()`
  - `agentfs_tool_query_file()`

对外接口主要有：

- `agent_file_init()`
- `agent_file_version_bump()`
- `inode_summary_refresh()`
- `agentfs_tool_set_file_attr()`
- `agentfs_tool_get_file_attr()`
- `agentfs_tool_del_file_attr()`
- `agentfs_tool_query_file()`
- `agent_notify_file_modified()`

这个文件的重点是让评审一眼看到：

- 元数据确实挂在真实 inode 上
- 查询不是遍历所有文件逐项检查
- 有运行时缓存、索引和共享查询结果

### `kernel/agent_loop.c`

这是任务五的独立模块，负责 Agent Loop 的内核运行机制：

- `agent_signal_filemod()`
- `agent_signal_event_locked()`
- `agent_proc_heartbeat_set()`
- `agent_proc_heartbeat_stop()`
- `agent_proc_priority_set()`
- `agent_proc_watch()`
- `agent_proc_unwatch()`
- `agent_proc_watch_file()`
- `agent_proc_sched_set()`
- `agent_proc_wait()`
- `agent_schedule_score()`
- `agent_tick()`
- `agent_notify_file_modified()`

对外接口主要有：

- `agent_proc_heartbeat_set()`
- `agent_proc_heartbeat_stop()`
- `agent_proc_watch()`
- `agent_proc_unwatch()`
- `agent_proc_watch_file()`
- `agent_proc_wait()`
- `agent_proc_priority_set()`
- `agent_proc_sched_set()`
- `agent_tick()`
- `agent_signal_filemod()`

这个文件集中体现：

- 心跳机制
- 消息/文件修改事件
- Agent Loop 生命周期等待
- 事件感知调度评分

### `kernel/agent_tool.c`

这是任务二和动态工具创新点的独立模块，负责工具调用与扩展机制：

- 内建工具：
  - `tool_get_system_status()`
  - `tool_query_process()`
  - `tool_send_message()`
  - `tool_read_context()`
- 工具分发：
  - `agent_tool_call()`
  - `agent_copy_tool_list()`
- 动态工具机制：
  - `agent_tool_register()`
  - `agent_tool_recv()`
  - `agent_tool_reply()`
  - `agent_dynamic_tool_call()`
- 动态工具全局状态：
  - `struct agent_dynamic_tool`
  - `struct agent_dynamic_request_slot`
  - `dynamic_tools[]`
  - `dynamic_requests[]`

对外接口主要有：

- `agent_tool_call()`
- `agent_tool_register()`
- `agent_tool_recv()`
- `agent_tool_reply()`
- `agent_copy_tool_list()`
- `agent_dynamic_tool_call()`

这个文件最适合讲“动态工具注册”这个创新点，因为注册表、请求槽、中转和回复都集中在这里。

### `kernel/sysagent.c`

这个文件不是 Agent 语义实现，而是系统调用边界层，负责：

- `argint` / `argaddr` / `argstr`
- `copyin` / `copyout`
- 调用 `agent*.c` 中的内核实现函数

它直接暴露给系统调用表的接口主要包括：

- `sys_agent_create()`
- `sys_agent_info()`
- `sys_agent_context_push()`
- `sys_agent_context_query()`
- `sys_agent_context_clear()`
- `sys_agent_context_rollback()`
- `sys_agent_tool_call()`
- `sys_agent_tool_list()`
- `sys_agent_tool_register()`
- `sys_agent_tool_recv()`
- `sys_agent_tool_reply()`
- `sys_agent_heartbeat_set()`
- `sys_agent_heartbeat_stop()`
- `sys_agent_watch()`
- `sys_agent_unwatch()`
- `sys_agent_wait()`
- `sys_agent_priority_set()`
- `sys_agent_sched_set()`

可以把它理解为：

```text
用户态参数解析层
```

而不是：

```text
Agent 功能实现层
```

## 3. 模块之间的调用关系

按调用方向看，当前结构大致是：

```text
user/*.c
  -> user.h / usys.S
  -> kernel/sysagent.c
  -> kernel/agent.c / agent_context.c / agent_fs.c / agent_loop.c / agent_tool.c
  -> kernel/proc.c / trap.c / file.c / fs.c
```

其中几个关键关系是：

- `sysagent.c` 只做参数解析和 `copyin/copyout`，再转调各个 `agent_*.c`
- `agent_tool.c` 会调用 `agent_context.c` 读取上下文，也会调用 `agent_fs.c` 执行 `query_file`
- `agent_loop.c` 依赖 `proc.c` / `trap.c` 提供时钟推进与调度接入
- `agent_fs.c` 依赖真实 inode、文件层和路径遍历逻辑维护属性、摘要与索引

## 4. 头文件关系

### `kernel/agent.h`

这是 Agent-OS 的公共 ABI 头文件，主要放：

- 常量定义
- 结构体定义
- 系统调用相关接口声明
- 各个 `agent_*.c` 模块之间共享的函数声明

用户态和内核态都会用到它，所以它是整个 Agent-OS 的接口中枢。

## 5. 评审/答辩推荐讲法

如果要快速讲清楚现在的结构，建议按下面顺序：

```text
第一层：sysagent.c
  负责 syscall 边界

第二层：5 个 agent 模块
  agent.c         核心元信息
  agent_context.c Context Path
  agent_fs.c      AgentFS
  agent_loop.c    Agent Loop
  agent_tool.c    Tool Call + 动态工具

第三层：proc.c / trap.c / file.c
  分别接入生命周期、时钟心跳和文件修改事件
```

如果评审问“为什么要拆”，一个直接回答是：

```text
拆分后每个文件基本对应一类赛题能力，
既方便定位实现，也方便证明模块边界和代码质量。
```

## 6. 当前阅读建议

如果只是想按功能看代码，推荐顺序：

```text
先看 kernel/agent.h
再看 kernel/sysagent.c
然后按任务看：
  任务一/三  -> agent.c + agent_context.c
  任务二    -> agent_tool.c
  任务四    -> agent_fs.c
  任务五    -> agent_loop.c
最后看 proc.c / trap.c / file.c 的接入点
```
