# Agent 内核模块总览

本文档根据当前仓库的实际代码结构，说明 Agent-OS 内核侧几个核心文件分别负责什么，方便阅读源码、答辩讲解和后续继续维护。

本文档同时记录当前已经暴露给用户态和宿主机侧的主要功能接口，包括：

- Agent 管理 syscall
- Context Path 接口
- Tool Call 接口
- AgentFS 文件语义查询接口
- Agent Loop 心跳/事件/调度接口
- 动态工具注册接口
- Task 6 CodeLab 场景入口
- LLM bridge/proxy 宿主机接口

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

当前内建工具包括：

```text
get_system_status()
query_process(type)
send_message(target_pid,message)
read_context()
read_file(path)
patch_file(path,op,old,new)
run_rule_test(target)
diff_file(path)
set_file_attr(path,key,value)
get_file_attr(path,key)
del_file_attr(path,key)
query_file(type,module,tag,keyword,public,mode)
```

其中 Task 6 CodeLab 主链路主要用：

```text
query_file(type=code;module=todo;keyword=delete)
read_file(path=repo/todo.c)
patch_file(path=repo/todo.c;op=replace;old=...;new=...)
diff_file(path=repo/todo.c)
send_message(target_pid=...;message=...)
```

`run_rule_test` 仍然是内建工具，但 Task 6 当前展示的是动态工具版本：

```text
run_rule_test_dyn
```

它由用户态 `rule_test_tool_agent` 运行时注册。

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

## 5. 用户态 syscall 接口速查

用户态声明位于：

```text
user/user.h
```

系统调用 stub 由：

```text
user/usys.pl
```

生成。

### Agent 创建和信息

```c
uint64 agent_create(int type, int heartbeat_interval, uint64 quota);
int agent_info(void *info);
```

用途：

- `agent_create`：把当前进程标记为 Agent，并初始化 Context 区、心跳、资源配额和调度字段。
- `agent_info`：读取当前 Agent 的状态，包括 `loop_state`、`sched_priority`、`sched_quota`、Context 长度等。

常见类型：

```text
AGENT_TYPE_PRIMARY
AGENT_TYPE_WORKER
```

### Context Path

```c
int context_push(void *node);
int context_query(void *buf, uint64 len);
int context_rollback(uint64 keep_nodes);
int context_clear(void);
```

用途：

- `context_push`：追加一次“请求/结果”记录。
- `context_query`：读取当前 Agent 的 Context Path。
- `context_rollback`：回滚到指定节点数。
- `context_clear`：清空 Context。

### Tool Call

```c
int tool_call(void *req, void *resp);
int tool_list(void *buf, uint64 len);
```

请求结构：

```c
struct agent_tool_request {
  char tool[AGENT_TOOL_NAME_MAX];
  char params[AGENT_TOOL_PARAM_MAX];
};
```

响应结构：

```c
struct agent_tool_response {
  int status;
  uint32 result_len;
  char result[AGENT_TOOL_RESULT_MAX];
};
```

参数格式统一使用：

```text
key=value;key=value
```

例如：

```text
tool=query_file
params=type=code;module=todo;keyword=delete
```

### Agent Loop 心跳和事件

```c
int agent_heartbeat_set(int interval);
int agent_heartbeat_stop(void);
int agent_watch(int mask);
int agent_unwatch(int mask);
int agent_watch_file(const char *path);
int agent_wait(int continue_loop, void *event);
```

事件类型：

```text
AGENT_EVENT_HEARTBEAT
AGENT_EVENT_MESSAGE
AGENT_EVENT_FILEMOD
```

用途：

- `agent_heartbeat_set`：设置心跳周期。
- `agent_watch`：关注 MESSAGE 或 FILEMOD 等事件。
- `agent_watch_file`：关注某个文件的修改事件。
- `agent_wait`：挂起等待事件；`continue_loop=0` 时表示任务完成，进入 DONE 状态。

### 调度接口

```c
int agent_priority_set(int priority);
int agent_sched_set(int priority, int quota);
```

当前范围：

```text
priority: 1..8
quota:    1..8
```

说明：

`agent_sched_set` 设置的是当前进程自己的调度参数，不是按 pid 修改其他进程。Task 6 中各 Agent 在启动后自行设置调度参数，再向 Planner 回报。

### 动态工具接口

```c
int tool_register(const char *name, int flags);
int tool_recv(void *req);
int tool_reply(int request_id, const char *result, int status);
```

典型流程：

```text
工具服务进程:
  agent_create(...)
  tool_register("run_rule_test_dyn", AGENT_TOOL_FLAG_PUBLIC)
  tool_recv(&req)
  处理 req.params
  tool_reply(req.request_id, result, AGENT_TOOL_OK)

调用方:
  tool_call("run_rule_test_dyn", "target=todo_delete")
```

Task 6 中的动态工具服务：

```text
user/rule_test_tool_agent.c
```

注册工具名：

```text
run_rule_test_dyn
```

## 6. Task 6 CodeLab 用户态程序

当前综合场景相关用户态程序：

```text
planner_agent
retriever_agent
patch_agent
test_agent
reviewer_agent
rule_test_tool_agent
llm_bridge
```

### `planner_agent`

主控 Agent。默认运行：

```text
planner_agent
```

LLM demo：

```text
planner_agent llm-demo
```

真实 API 主控模式：

```text
planner_agent llm-api
```

在 `llm-api` 模式下，Planner 会输出：

```text
@@AGENTOS_LLM_REQ id=planner role=planner prompt=fix todo delete bug @@END
```

并等待宿主机 proxy 回写：

```text
@@AGENTOS_LLM_RESP id=planner state=done text=... action=start_codelab @@END
```

当 `action=start_codelab` 时，Planner 创建 Retriever/Patch/Test/Reviewer/Tool-Service 并启动完整修复链路。

当 `action=abort` 时，Planner 不启动修复链路。

### `retriever_agent`

负责：

```text
query_file -> read_file -> found_bug -> send patch request
```

同时第一次 `query_file` 会填充 shared query cache。

### `patch_agent`

负责：

```text
patch_file repo/todo.c
```

修改完成后触发 FILEMOD，并通知 Test-Agent。

### `test_agent`

负责调用动态工具：

```text
run_rule_test_dyn(target=todo_delete)
```

### `reviewer_agent`

负责：

```text
watch_file(repo/todo.c)
重复 query_file 展示 cache_hit=1
diff_file(repo/todo.c)
approve / reject
```

### `rule_test_tool_agent`

动态工具服务。注册：

```text
run_rule_test_dyn
```

并返回：

```text
{status=ok,tool=run_rule_test_dyn,target=todo_delete,checks=CDT,passed=3,total=3}
```

### `llm_bridge`

独立 bridge 测试程序：

```text
llm_bridge demo fix_todo_delete
llm_bridge api fix_todo_delete
```

它用于单独验证 xv6 与宿主机 proxy 的请求/响应协议。

## 7. 宿主机侧 LLM 接口

宿主机脚本位于：

```text
tools/llm_proxy.py
tools/llm_qemu_driver.py
```

### `llm_proxy.py`

作用：

- 解析 `@@AGENTOS_LLM_REQ ... @@END`
- demo 模式返回固定响应
- api 模式调用第三方 OpenAI-compatible HTTP API
- 输出短格式 `@@AGENTOS_LLM_RESP ... @@END`

demo 测试：

```bash
printf '@@AGENTOS_LLM_REQ id=1 role=planner prompt=fix_todo @@END\n' | \
python3 tools/llm_proxy.py --mode demo
```

真实 API 环境变量：

```bash
export AGENTOS_LLM_API_KEY="你的第三方 API key"
export AGENTOS_LLM_MODEL="你的模型名称"
export AGENTOS_LLM_API_URL="第三方 chat completions URL"
```

真实 API 单测：

```bash
printf '@@AGENTOS_LLM_REQ id=1 role=planner prompt=fix_todo @@END\n' | \
python3 tools/llm_proxy.py --mode api
```

### `llm_qemu_driver.py`

作用：

- 自动启动 QEMU
- 自动运行 `planner_agent llm-api`
- 捕获 Planner 发出的 `@@AGENTOS_LLM_REQ`
- 调用 `llm_proxy.py` 的 demo/api 逻辑
- 把 `@@AGENTOS_LLM_RESP` 回写给 xv6
- 让真实模型参与完整 CodeLab 修复主流程

demo 自动联动：

```bash
python3 tools/llm_qemu_driver.py --mode demo
```

真实 API 自动联动：

```bash
python3 tools/llm_qemu_driver.py --mode api
```

如果 `fs.img` 被其他 QEMU 占用：

```bash
python3 tools/llm_qemu_driver.py --mode api \
  --disk-image /tmp/agentos-full-codelab-driver.img
```

当前 LLM 的作用：

```text
LLM 负责决策：是否 action=start_codelab
AgentOS 负责执行：query/read/patch/test/review
```

这体现的是：

```text
LLM 是策略层
AgentOS 是受控执行层
```

## 8. 预置仓库与文件接口

Task 6 使用的预置代码仓库：

```text
repo/main.c
repo/todo.c
repo/todo.h
repo/test.c
repo/README
```

`mkfs/mkfs.c` 会把这些文件打进 `fs.img`，并为它们写入 AgentFS 元数据。

典型元数据：

```text
repo/todo.c   type=code module=todo tag=delete
repo/todo.h   type=code module=todo tag=api
repo/test.c   type=test module=todo tag=delete
repo/README   type=doc  module=todo tag=requirement
repo/main.c   type=code module=todo tag=entry
```

语义查询示例：

```text
query_file(type=code;module=todo;keyword=delete)
```

## 9. 输出规范

Task 6 当前输出采用分角色标签：

```text
[Planner-Agent]
[Retriever-Agent]
[Patch-Agent]
[Test-Agent]
[Reviewer-Agent]
[Tool-Service]
[Kernel]
[Kernel-AgentLoop]
[Kernel-FS]
[Scheduler]
[Summary]
[Context]
[Agent-Loop]
[LLM-Bridge]
[Host-Proxy]
```

关键成功标志：

```text
[Summary] shared cache: retriever first query cache_hit=0; reviewer repeated query cache_hit=1
[Summary] dynamic tool: run_rule_test_dyn registered and used by Test-Agent
[Summary] model: llm-api approved start_codelab
[Agent-Loop] final loop_state=5
```

## 10. 评审/答辩推荐讲法

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

## 11. 当前阅读建议

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
