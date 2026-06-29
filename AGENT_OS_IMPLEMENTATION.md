# Agent-OS 实现说明

本文档说明当前仓库中已经实现的 AgentOS 代码结构、核心能力、系统调用和测试方式。文档定位是代码说明，不包含规划性内容。

## 1. 实现总览

AgentOS 在 xv6 基础上增加了面向 Agent 的内核能力：

| 能力 | 实现结果 |
| --- | --- |
| Agent 进程模型 | 普通进程可通过 `agent_create` 标记为 Agent，PCB 中保存 Agent 类型、权限组、Context、事件和调度字段 |
| Agent Context | 每个 Agent 拥有 Context Path，可记录工具调用、观察结果和推理路径 |
| Tool Call | 支持内置工具调用，也支持用户态工具服务动态注册 |
| AgentFS | 支持 inode 元数据、属性查询、摘要匹配、索引查询和共享查询缓存 |
| Agent Loop | 支持心跳、消息事件、文件修改事件、等待/唤醒和 DONE 生命周期 |
| 事件感知调度 | 调度分数结合 priority、quota、pending event 和 aging |
| CodeLab 场景 | 多 Agent 协作修复 `/repo/todo.c`，展示 AgentFS、动态工具、调度、事件和 LLM bridge |

## 2. 内核模块结构

```text
kernel/agent.h
  AgentOS 公共 ABI、常量、结构体和内核函数声明。

kernel/agent.c
  Agent 核心逻辑：
  agent_create / agent_info / Agent 默认字段初始化 / fork 继承。

kernel/agent_context.c
  Agent Context 区管理：
  context_push / context_query / context_rollback / context_clear。

kernel/agent_tool.c
  Tool Call 和动态工具：
  内置工具分发、动态工具注册、请求转发、工具回复、进程退出清理。

kernel/agent_fs.c
  AgentFS 文件系统扩展：
  inode 元数据、文件属性、摘要、倒排索引、query_file、shared query cache。

kernel/agent_loop.c
  Agent Loop：
  心跳、消息事件、文件修改事件、agent_wait、事件权重、调度评分。

kernel/sysagent.c
  Agent syscall 入口：
  负责用户参数读取、copyin/copyout，并调用对应内核模块函数。

kernel/proc.h
  在 struct proc 中增加 Agent 字段：
  Agent 类型、group、Context、事件状态、心跳、调度 priority/quota。

kernel/proc.c
  进程生命周期和调度接入：
  allocproc/freeproc/fork/exit 初始化和清理 Agent 状态；
  scheduler 调用 agent_schedule_score()。

kernel/trap.c
  时钟中断中调用 agent_tick()，驱动心跳事件。

kernel/syscall.h, kernel/syscall.c
  Agent syscall 编号和 syscall 分发表。
```

## 3. 用户态程序结构

```text
user/agenttest.c
  Agent 基础能力、Context、Tool Call、AgentFS 查询测试。

user/agentlooptest.c
  心跳、消息事件、文件修改事件、agent_wait、多 Agent 调度测试。

user/agentfsbench.c
  AgentFS 索引查询和遍历查询性能对比。

user/agentperftest.c
  AgentOS 性能测试。

user/agentinnovationtest.c
  共享查询缓存、事件感知调度、动态工具注册三个创新点测试。

user/planner_agent.c
  CodeLab 主 Agent，总控修复流程。

user/retriever_agent.c
  检索 Agent，通过 query_file/read_file 定位 bug。

user/patch_agent.c
  修复 Agent，修改 repo/todo.c。

user/test_agent.c
  测试 Agent，调用动态工具 run_rule_test_dyn。

user/reviewer_agent.c
  审核 Agent，监听 FILEMOD 并审核修复结果。

user/rule_test_tool_agent.c
  动态工具服务，注册 run_rule_test_dyn。

user/llm_bridge.c
  xv6 用户态 LLM bridge，通过串口协议和宿主机 proxy 通信。

tools/llm_qemu_driver.py
  宿主机 QEMU driver，自动启动 QEMU 并驱动 LLM bridge。

tools/llm_proxy.py
  宿主机 LLM proxy，支持 demo 模式和第三方 API 模式。
```

## 4. 系统调用接口

用户态声明位于 `user/user.h`，stub 由 `user/usys.pl` 生成。

| syscall | 功能 |
| --- | --- |
| `agent_create(type, flags, ctx_pages)` | 把当前进程标记为 Agent 并分配 Context 区 |
| `agent_info(buf)` | 读取当前进程 Agent 状态 |
| `context_push(req)` | 追加一条 Context Path 节点 |
| `context_query(buf, len)` | 读取 Context Path |
| `context_rollback(n)` | 回滚最近 n 条 Context 节点 |
| `context_clear()` | 清空 Context Path |
| `tool_call(req, resp)` | 调用内置或动态工具 |
| `tool_list(buf, len)` | 列出当前可见工具 |
| `tool_register(name, flags)` | 用户态工具服务注册动态工具 |
| `tool_recv(req)` | 工具服务接收调用请求 |
| `tool_reply(id, result, status)` | 工具服务回复调用结果 |
| `agent_heartbeat_set(period)` | 设置 Agent 心跳周期 |
| `agent_heartbeat_stop()` | 停止心跳 |
| `agent_watch(mask)` | 关注事件类型 |
| `agent_unwatch(mask)` | 取消关注事件 |
| `agent_watch_file(path)` | 关注指定文件修改事件 |
| `agent_wait(loop_state, event_out)` | 进入 Agent Loop 等待心跳或事件 |
| `agent_priority_set(priority)` | 设置 Agent 优先级 |
| `agent_sched_set(priority, quota)` | 设置 Agent 调度 priority/quota |

## 5. Agent 进程模型

Agent 进程由 `agent_create()` 创建，核心状态保存在 `struct proc` 中：

```text
agent_enabled
agent_type
agent_group
agent_flags
agent_context_base
agent_context_pages
agent_context_len
pending_events
watched_events
heartbeat_period
heartbeat_next
agent_priority
agent_quota
loop_state
```

`AGENT_TYPE_PRIMARY` 会创建新的 `agent_group`。子 Agent 通过 fork/exec 继承 group，使同组 Agent 可以共享查询缓存和默认访问同组动态工具。

## 6. Context Path

Context Path 用于记录 Agent 的执行轨迹：

```text
req=工具调用或动作
res=观察结果或返回值
ts=内核时间戳
```

实现位置：

```text
kernel/agent_context.c
```

主要能力：

| 能力 | 说明 |
| --- | --- |
| 追加 | `context_push()` 写入一条路径节点 |
| 查询 | `context_query()` 返回最近路径 |
| 回滚 | `context_rollback()` 删除最近 n 条 |
| 清空 | `context_clear()` 清空全部路径 |
| 淘汰 | 超过容量时 FIFO 淘汰旧节点 |

## 7. Tool Call 与动态工具

Tool Call 使用结构化请求和响应。内核中的 Tool Router 位于：

```text
kernel/agent_tool.c
```

内置工具包括：

| 工具 | 功能 |
| --- | --- |
| `get_system_status` | 查询系统状态 |
| `query_process` | 查询进程/Agent 列表 |
| `send_message` | 向目标 Agent 发送消息并触发 MESSAGE 事件 |
| `read_context` | 读取目标 Agent 的 Context |
| `set_file_attr` | 设置文件属性 |
| `get_file_attr` | 读取文件属性 |
| `del_file_attr` | 删除文件属性 |
| `query_file` | 按属性和摘要查询文件 |
| `read_file` | 读取文件内容 |
| `patch_file` | 修改文件内容 |
| `diff_file` | 查看修复前后差异 |

动态工具流程：

```text
Tool Service:
  tool_register("run_rule_test_dyn", AGENT_TOOL_FLAG_PUBLIC)
  tool_recv()
  tool_reply()

Calling Agent:
  tool_call("run_rule_test_dyn", "target=todo_delete")
```

内核维护：

```text
dynamic_tools[]
dynamic_requests[]
```

工具进程退出时，`agent_tool_cleanup_proc()` 会清理工具表和未完成请求。

## 8. AgentFS

AgentFS 实现位置：

```text
kernel/agent_fs.c
```

已经实现的能力：

| 能力 | 说明 |
| --- | --- |
| inode 元数据 | 为真实 inode 维护 type/owner/tag/module/priority 等属性 |
| 属性设置 | `set_file_attr` 设置属性 |
| 属性读取 | `get_file_attr` 读取属性 |
| 属性删除 | `del_file_attr` 删除属性 |
| 摘要维护 | 维护文件摘要和关键词信息 |
| 倒排索引 | 属性值映射到候选 inode 集合 |
| 多条件查询 | `query_file(type=code,module=todo,keyword=delete)` |
| 结构化结果 | 返回 path、属性、摘要、cache_hit、used_index |
| 共享缓存 | 多 Agent 复用相同查询结果 |

查询路径：

```text
query_file
  -> shared_query_cache_lookup
  -> file_index_ensure
  -> 属性倒排索引缩小候选集合
  -> file_meta_matches 精确匹配
  -> shared_query_cache_store
```

## 9. Agent Loop

Agent Loop 实现位置：

```text
kernel/agent_loop.c
```

状态循环：

```text
WAITING
  -> HEARTBEAT / MESSAGE / FILEMOD 唤醒
  -> THINK
  -> ACT
  -> OBSERVE
  -> CONTINUE 或 DONE
```

事件类型：

| 事件 | 来源 |
| --- | --- |
| `AGENT_EVENT_HEARTBEAT` | `agent_tick()` 根据 heartbeat period 触发 |
| `AGENT_EVENT_MESSAGE` | `send_message` 写入消息并唤醒目标 Agent |
| `AGENT_EVENT_FILEMOD` | 文件属性或内容修改后唤醒 watcher |

`agent_wait(loop_state, event_out)` 在没有 pending event 时让 Agent 睡眠。事件到达后，内核设置 pending event 并唤醒进程。

## 10. 事件感知调度

调度评分由 `agent_schedule_score()` 计算，scheduler 在 `kernel/proc.c` 中调用。

```text
score = priority * 10
      + quota
      + event_weight(pending_events)
      + aging_bonus
```

事件权重位于 `agent_event_weight()`：

```text
MESSAGE  >  FILEMOD  >  HEARTBEAT
```

效果：

| 场景 | 调度效果 |
| --- | --- |
| Agent 收到消息 | 优先响应外部请求 |
| Agent 监听文件修改 | 文件变化后快速审核或处理 |
| Agent 仅心跳唤醒 | 周期性运行，但不会长期抢占 |
| 普通进程等待较久 | aging 防止饥饿 |

## 11. 三个创新点实现位置

| 创新点 | 主要文件 | 关键实现 |
| --- | --- | --- |
| 跨 Agent 共享查询缓存 | `kernel/agent_fs.c` | `shared_query_cache_lookup/store`、权限检查、版本失效 |
| 事件感知调度 | `kernel/agent_loop.c`、`kernel/proc.c` | `agent_event_weight`、`agent_schedule_score`、scheduler 接入 |
| 动态工具注册 | `kernel/agent_tool.c` | `dynamic_tools`、`dynamic_requests`、`tool_register/recv/reply` |

## 12. CodeLab 综合场景

预置仓库位于：

```text
repo/main.c
repo/todo.c
repo/todo.h
repo/test.c
repo/README
```

多 Agent 分工：

| Agent | 功能 |
| --- | --- |
| Planner-Agent | 总控流程，创建 worker，输出 summary |
| Retriever-Agent | 使用 AgentFS 查询并读取目标文件 |
| Patch-Agent | 修改 `repo/todo.c` |
| Test-Agent | 调用动态工具 `run_rule_test_dyn` |
| Reviewer-Agent | 监听 FILEMOD，审核 diff 和测试结果 |
| Tool-Service | 注册并执行动态规则测试工具 |

演示链路：

```text
Planner heartbeat wakeup
  -> spawn workers
  -> Retriever query_file cache_hit=0
  -> Reviewer repeated query cache_hit=1
  -> Patch patch_file
  -> FILEMOD wake Reviewer
  -> Test tool_call run_rule_test_dyn
  -> Reviewer approve
  -> Planner summary
  -> loop_state DONE
```

## 13. LLM Bridge

LLM bridge 由 xv6 用户态和宿主机脚本组成：

```text
user/llm_bridge.c
tools/llm_qemu_driver.py
tools/llm_proxy.py
```

通信协议：

```text
@@AGENTOS_LLM_REQ id=planner role=planner prompt=fix todo delete bug @@END
@@AGENTOS_LLM_RESP id=planner state=done text=... action=start_codelab @@END
```

`planner_agent llm-api` 会先通过 bridge 请求宿主机模型决策。LLM 返回 `action=start_codelab` 后，Planner 执行同一套 CodeLab 多 Agent 流程。

## 14. 编译与运行

在宿主机执行：

```bash
make clean
make
make fs.img
make qemu
```

进入 xv6 shell 后运行基础测试：

```text
agenttest
agentlooptest
agentfsbench
agentinnovationtest
```

性能测试：

```text
agentperftest
```

CodeLab 规则版：

```text
planner_agent
```

CodeLab LLM demo 版：

```text
planner_agent llm-demo
```

宿主机自动驱动 QEMU 的 demo 版：

```bash
python3 tools/llm_qemu_driver.py --mode demo
```

宿主机自动驱动 QEMU 的真实 API 版：

```bash
export AGENTOS_LLM_API_KEY="你的第三方 API key"
export AGENTOS_LLM_MODEL="你的模型名称"
export AGENTOS_LLM_API_URL="第三方 chat completions 接口地址"
python3 tools/llm_qemu_driver.py --mode api
```

## 15. 预期测试结果

基础测试成功时输出：

```text
agenttest: all tests passed
agentlooptest: all tests passed
agentfsbench: all tests passed
agentinnovationtest: all tests passed
```

CodeLab 成功输出包含：

```text
[Kernel-FS] query_file type=code,module=todo,keyword=delete -> cache_hit=0, used_index=1
[Kernel-FS] reviewer repeated query -> shared query cache hit
[Tool-Service] register run_rule_test_dyn ok
[Test-Agent] tool_call run_rule_test_dyn -> {status=ok,...passed=3,total=3}
[Reviewer-Agent] diff_file + rule result -> approve
[Summary] dynamic tool: run_rule_test_dyn registered and used by Test-Agent
[Agent-Loop] final loop_state=5
```
