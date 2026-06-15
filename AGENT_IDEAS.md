
# Agent 创新想法实现状态

当前三条想法均已落地：

| 想法 | 状态 | 主要实现位置 | 演示 |
| --- | --- | --- | --- |
| 跨 Agent 的共享查询结果 | 已实现 | `kernel/agent.c` 的 `shared_query_cache`、`query_file` 缓存命中/失效逻辑 | `agentinnovationtest` 的 `shared_cache_test` |
| 事件感知调度 | 已实现 | `kernel/proc.c` scheduler、`agent_schedule_score()`、`AGENT_EVENT_FILEMOD` | `agentinnovationtest` 的 `event_scheduler_test` |
| 工具能力动态注册 | 已实现 | `tool_register` / `tool_recv` / `tool_reply` 和动态请求表 | `agentinnovationtest` 的 `dynamic_tool_test` |

详细实现统一见 `AGENT_OS_IMPLEMENTATION.md`，演示步骤见 `AGENT_OS_INTERFACE_LLM_GUIDE.md`。


# 1：跨 Agent 的共享查询结果

赛题开放创新方向里明确提到“上下文路径的跨进程共享，多 Agent 共享查询结果，避免重复查询”。

这个比普通 Context Path 更有创新性。

## 场景

多个 Agent 都要查询：

```
query_file(type=log, tag=error)
```

如果每个 Agent 都查一次，浪费。

你可以做一个简单的共享缓存：

```
struct shared_query_cache {
  char query[64];
  struct result results[8];
  int owner_pid;
  int refcnt;
  uint64 version;
};
```

当 Agent-B 查询相同条件时：

```
[Kernel] shared cache hit from Agent-A
```

## 加安全控制

只允许共享：

```
public=true
owner=system
或者同组 Agent
```

这就和“安全隔离”结合了。

## 数据

| 场景     | Agent 数 | query_file 调用次数 | 实际 FS 查询次数 |
| -------- | -------- | ------------------- | ---------------- |
| 无共享   | 5        | 5                   | 5                |
| 共享缓存 | 5        | 5                   | 1                |

这是非常强的创新点，但实现比前几个稍微复杂。



# 2：事件感知调度，而不只是 Agent 优先级

任务五里“优先级机制”只是可选，但你可以做得更有特色：**调度不仅看 Agent priority，还看唤醒它的事件类型。**

基础做法：

```
Agent A priority=5
Agent B priority=5
随机/轮转调度
```

你的创新做法：

```
收到 MESSAGE 的 Agent > FILEMOD 唤醒的 Agent > HEARTBEAT 唤醒的 Agent
```

因为对 Agent 来说：

```
消息事件通常比周期心跳更紧急。
```

## 可以定义事件权重

```
int event_weight(int pending_event) {
  if(pending_event & AGENT_EVENT_MESSAGE) return 30;
  if(pending_event & AGENT_EVENT_FILEMOD) return 20;
  if(pending_event & AGENT_EVENT_HEARTBEAT) return 10;
  return 0;
}
```

调度时使用：

```
score = agent_priority * 10 + event_weight(pending_event) + aging_bonus;
```

## 这比普通优先级强在哪里？

普通优先级是静态的。

事件感知调度是动态的：

```
同一个 Agent，收到紧急消息时优先级临时升高；
只是普通心跳时不会抢占太多 CPU。
```

## 你可以测什么？

创建 3 个 Agent：

```
Agent-H：心跳唤醒
Agent-F：文件修改唤醒
Agent-M：消息唤醒
```

同时变为 RUNNABLE，看谁先运行。

输出：

```
[Scheduler] run Agent-M event=MESSAGE score=80
[Scheduler] run Agent-F event=FILEMOD score=70
[Scheduler] run Agent-H event=HEARTBEAT score=60
```

这是真正高于赛题要求的机制。



# 3：工具能力动态注册

赛题开放创新方向也提到“Agent 工具能力的动态注册”。

基础任务二是内核固定工具：

```
query_process
query_file
send_message
```

创新版本是：

```
用户态服务可以注册一个工具名；
Agent 调用 tool_call 时，内核路由到该服务。
```

例如：

```
register_tool("summarize_log", pid);
```

Agent 调用：

```
tool_call("summarize_log", "file=error_log")
```

内核发现：

```
summarize_log belongs to pid=7
```

于是发消息给工具服务进程，等待结果。

## 这个创新很强，但风险也高

因为要做：

```
工具表
权限检查
请求转发
结果返回
超时处理
```

如果时间有限，不建议优先做。
