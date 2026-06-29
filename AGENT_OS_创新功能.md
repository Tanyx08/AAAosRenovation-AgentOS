# AgentOS 创新功能说明

本文档说明当前仓库已经完成的 AgentOS 创新功能，包括实现位置、核心机制和运行验证方式。

## 1. 创新功能总览

| 创新功能 | 状态 | 主要实现位置 | 验证程序 |
| --- | --- | --- | --- |
| 跨 Agent 共享查询缓存 | 已完成 | `kernel/agent_fs.c` | `agentinnovationtest`、`planner_agent` |
| 事件感知调度 | 已完成 | `kernel/agent_loop.c`、`kernel/proc.c` | `agentlooptest`、`agentinnovationtest`、`planner_agent` |
| 动态工具注册 | 已完成 | `kernel/agent_tool.c` | `agentinnovationtest`、`rule_test_tool_agent`、`test_agent` |

## 2. 跨 Agent 共享查询缓存

### 2.1 功能说明

多个 Agent 对同一批文件执行相同或可共享的 `query_file` 查询时，内核会复用第一次查询结果，避免每个 Agent 都重新扫描 AgentFS 元数据。

CodeLab 场景中：

```text
Retriever-Agent 第一次查询:
  query_file(type=code,module=todo,keyword=delete)
  -> cache_hit=0

Reviewer-Agent 重复查询:
  query_file(type=code,module=todo,keyword=delete)
  -> cache_hit=1
```

### 2.2 实现位置

```text
kernel/agent_fs.c
```

关键结构和函数：

```text
struct shared_query_cache
shared_query_cache_lookup()
shared_query_cache_store()
cache_access_allowed()
query_result_with_cache()
agent_file_version_bump()
```

### 2.3 实现机制

查询流程：

```text
query_file
  -> shared_query_cache_lookup
  -> 命中则直接返回结构化结果
  -> 未命中则走 AgentFS 索引查询
  -> shared_query_cache_store 保存结果
```

缓存安全规则：

```text
同一 Agent group 可共享
public 查询结果可共享
文件属性版本变化后缓存失效
```

返回结果中包含：

```text
cache_hit
used_index
path
summary
type / owner / tag / module
```

### 2.4 运行验证

进入 xv6 shell 后运行：

```text
agentinnovationtest
```

也可以运行 CodeLab 场景：

```text
planner_agent
```

预期关键输出：

```text
[Kernel-FS] query_file type=code,module=todo,keyword=delete -> cache_hit=0, used_index=1
[Kernel-FS] reviewer repeated query -> shared query cache hit
[Summary] shared cache: retriever first query cache_hit=0; reviewer repeated query cache_hit=1
```

## 3. 事件感知调度

### 3.1 功能说明

AgentOS 的调度不只依赖静态优先级，还会根据 Agent 当前等待的事件类型动态调整调度分数。收到消息、文件修改事件或心跳事件的 Agent，会获得不同事件权重。

事件优先级：

```text
MESSAGE  >  FILEMOD  >  HEARTBEAT
```

### 3.2 实现位置

```text
kernel/agent_loop.c
kernel/proc.c
kernel/proc.h
kernel/trap.c
```

关键函数：

```text
agent_event_weight()
agent_schedule_score()
agent_proc_sched_set()
agent_tick()
scheduler()
```

### 3.3 实现机制

调度评分：

```text
score = priority * 10
      + quota
      + event_weight(pending_events)
      + aging_bonus
```

事件来源：

```text
HEARTBEAT:
  trap.c 时钟 tick 调用 agent_tick()

MESSAGE:
  send_message 工具写入目标 Agent 消息并唤醒

FILEMOD:
  patch_file / set_file_attr / del_file_attr 修改文件后唤醒 watcher
```

CodeLab 中 Planner 会给不同角色设置不同调度参数：

```text
Planner-Agent    sched=4/3
Retriever-Agent  sched=8/8
Patch-Agent      sched=8/7
Test-Agent       sched=5/4
Reviewer-Agent   sched=7/6
Tool-Service     sched=4/3
```

### 3.4 运行验证

进入 xv6 shell 后运行：

```text
agentlooptest
agentinnovationtest
planner_agent
```

预期关键输出：

```text
[Scheduler] Planner-Agent sched=4/3 heartbeat=12
[Scheduler] Retriever-Agent high priority sched=8/8
[Scheduler] Patch-Agent high priority sched=8/7
[Scheduler] Test-Agent on-demand sched=5/4
[Scheduler] Reviewer-Agent filemod/message sched=7/6
```

## 4. 动态工具注册

### 4.1 功能说明

AgentOS 支持用户态工具服务在运行时注册工具。其他 Agent 不需要知道工具进程是谁，只需要通过 `tool_call` 调用工具名，内核 Tool Router 会查表并转发请求。

CodeLab 场景中，`rule_test_tool_agent` 注册动态工具：

```text
run_rule_test_dyn
```

`test_agent` 调用该工具完成规则测试。

### 4.2 实现位置

```text
kernel/agent_tool.c
kernel/sysagent.c
user/usys.pl
user/user.h
user/rule_test_tool_agent.c
user/test_agent.c
```

关键结构和函数：

```text
struct agent_dynamic_tool
struct agent_dynamic_request_slot
dynamic_tools[]
dynamic_requests[]
agent_tool_register()
agent_dynamic_tool_call()
agent_tool_recv()
agent_tool_reply()
agent_tool_cleanup_proc()
```

### 4.3 实现机制

注册流程：

```text
Tool-Service
  -> tool_register("run_rule_test_dyn", AGENT_TOOL_FLAG_PUBLIC)
  -> 内核写入 dynamic_tools[]
```

调用流程：

```text
Calling Agent
  -> tool_call("run_rule_test_dyn", "target=todo_delete")
  -> Kernel Tool Router 查 dynamic_tools[]
  -> 写入 dynamic_requests[]
  -> 唤醒 Tool-Service
  -> Tool-Service tool_recv()
  -> Tool-Service tool_reply()
  -> Calling Agent 收到响应
```

退出清理：

```text
工具服务进程退出
  -> agent_tool_cleanup_proc()
  -> 清理 dynamic_tools[]
  -> 唤醒等待中的 dynamic_requests[]
```

### 4.4 运行验证

进入 xv6 shell 后运行：

```text
agentinnovationtest
planner_agent
```

预期关键输出：

```text
[Tool-Service] register run_rule_test_dyn ok
[Test-Agent] tool_call run_rule_test_dyn -> {status=ok,tool=run_rule_test_dyn,target=todo_delete,checks=CDT,passed=3,total=3}
[Summary] dynamic tool: run_rule_test_dyn registered and used by Test-Agent
```

## 5. CodeLab 中的创新点联动

CodeLab 场景把三个创新点串成一条完整链路：

```text
Planner-Agent
  -> 设置各角色调度参数
  -> Retriever-Agent 查询 repo/todo.c，写入 shared query cache
  -> Reviewer-Agent 重复查询并命中 shared query cache
  -> Patch-Agent 修改 repo/todo.c，触发 FILEMOD
  -> Reviewer-Agent 被 FILEMOD 唤醒
  -> Test-Agent 调用动态工具 run_rule_test_dyn
  -> Planner-Agent 输出 summary
```

运行：

```text
planner_agent
```

LLM demo 版：

```text
planner_agent llm-demo
```

宿主机自动 QEMU demo：

```bash
python3 tools/llm_qemu_driver.py --mode demo
```

真实 API 版：

```bash
export AGENTOS_LLM_API_KEY="你的第三方 API key"
export AGENTOS_LLM_MODEL="你的模型名称"
export AGENTOS_LLM_API_URL="第三方 chat completions 接口地址"
python3 tools/llm_qemu_driver.py --mode api
```

## 6. 测试命令汇总

```text
agentlooptest
agentinnovationtest
agentfsbench
planner_agent
planner_agent llm-demo
```

基础成功结果：

```text
agentlooptest: all tests passed
agentinnovationtest: all tests passed
agentfsbench: all tests passed
```
