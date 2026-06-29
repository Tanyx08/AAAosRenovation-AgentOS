# AgentOS 接口与运行说明

本文档说明当前仓库已经实现的用户态接口、测试程序、CodeLab 场景和 LLM bridge 运行方式。文档定位是使用说明和接口说明。

## 1. 编译和启动

在宿主机执行：

```bash
make clean
make
make fs.img
make qemu
```

进入 xv6 shell 后可以运行：

```text
agenttest
agentlooptest
agentfsbench
agentperftest
agentinnovationtest
planner_agent
planner_agent llm-demo
```

## 2. 头文件

用户态 Agent 程序使用以下头文件：

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"
```

AgentOS ABI 常量和结构体位于：

```text
kernel/agent.h
```

用户态 syscall 声明位于：

```text
user/user.h
```

## 3. Agent 基础接口

| 接口 | 作用 |
| --- | --- |
| `agent_create(type, flags, ctx_pages)` | 创建当前进程的 Agent 身份 |
| `agent_info(buf)` | 读取当前 Agent 信息 |
| `agent_priority_set(priority)` | 设置 Agent 优先级 |
| `agent_sched_set(priority, quota)` | 设置 Agent 调度参数 |

常用 Agent 类型：

```c
AGENT_TYPE_NORMAL
AGENT_TYPE_PRIMARY
AGENT_TYPE_WORKER
```

示例：

```c
agent_create(AGENT_TYPE_PRIMARY, 0, 2);
agent_sched_set(4, 3);
```

## 4. Context Path 接口

| 接口 | 作用 |
| --- | --- |
| `context_push(req)` | 追加一条 Context 记录 |
| `context_query(buf, len)` | 读取 Context Path |
| `context_rollback(n)` | 回滚最近 n 条记录 |
| `context_clear()` | 清空 Context |

Context 用来记录 Agent 的执行链路，例如：

```text
req=query_file(type=code,module=todo,keyword=delete)
res=path=repo/todo.c;cache_hit=0;used_index=1
```

## 5. Tool Call 接口

| 接口 | 作用 |
| --- | --- |
| `tool_call(req, resp)` | 调用工具 |
| `tool_list(buf, len)` | 列出可用工具 |

内置工具：

| 工具名 | 功能 |
| --- | --- |
| `get_system_status` | 查询系统状态 |
| `query_process` | 查询进程和 Agent |
| `send_message` | 向 Agent 发送消息 |
| `read_context` | 读取 Agent Context |
| `set_file_attr` | 设置文件属性 |
| `get_file_attr` | 读取文件属性 |
| `del_file_attr` | 删除文件属性 |
| `query_file` | 按属性和摘要查询文件 |
| `read_file` | 读取文件 |
| `patch_file` | 修改文件 |
| `diff_file` | 查看文件 diff |

## 6. 动态工具注册接口

| 接口 | 作用 |
| --- | --- |
| `tool_register(name, flags)` | 工具服务进程注册工具 |
| `tool_recv(req)` | 工具服务接收请求 |
| `tool_reply(id, result, status)` | 工具服务返回结果 |

动态工具调用链：

```text
Tool-Service:
  tool_register("run_rule_test_dyn", AGENT_TOOL_FLAG_PUBLIC)
  tool_recv()
  tool_reply()

Test-Agent:
  tool_call("run_rule_test_dyn", "target=todo_delete")
```

当前 CodeLab 场景中使用的动态工具：

```text
run_rule_test_dyn
```

实现文件：

```text
kernel/agent_tool.c
user/rule_test_tool_agent.c
user/test_agent.c
```

## 7. AgentFS 接口

AgentFS 通过 Tool Call 暴露文件属性和查询能力。

| 工具 | 示例参数 | 结果 |
| --- | --- | --- |
| `set_file_attr` | `path=repo/todo.c;type=code;module=todo;tag=delete` | 设置 inode 元数据 |
| `get_file_attr` | `path=repo/todo.c` | 返回文件属性 |
| `del_file_attr` | `path=repo/todo.c;key=tag` | 删除属性 |
| `query_file` | `type=code,module=todo,keyword=delete` | 返回结构化查询结果 |
| `read_file` | `path=repo/todo.c` | 返回文件内容 |
| `patch_file` | `path=repo/todo.c;op=replace;...` | 修改文件 |
| `diff_file` | `path=repo/todo.c` | 返回修复差异 |

`query_file` 返回结果包含：

```text
path
type
owner
tag
module
summary
cache_hit
used_index
```

AgentFS 实现文件：

```text
kernel/agent_fs.c
```

## 8. Agent Loop 接口

| 接口 | 作用 |
| --- | --- |
| `agent_heartbeat_set(period)` | 设置心跳周期 |
| `agent_heartbeat_stop()` | 停止心跳 |
| `agent_watch(mask)` | 注册关注事件 |
| `agent_unwatch(mask)` | 取消关注事件 |
| `agent_watch_file(path)` | 关注文件修改 |
| `agent_wait(loop_state, event_out)` | 等待心跳或事件 |

事件类型：

```c
AGENT_EVENT_HEARTBEAT
AGENT_EVENT_MESSAGE
AGENT_EVENT_FILEMOD
```

常用等待流程：

```c
agent_heartbeat_set(10);
agent_watch(AGENT_WATCH_MESSAGE);
agent_wait(AGENT_LOOP_CONTINUE, &event);
```

实现文件：

```text
kernel/agent_loop.c
kernel/proc.c
kernel/trap.c
```

## 9. 消息事件

消息通过内置工具 `send_message` 发送。

```text
tool_call("send_message", "pid=4;message=role=retriever;path=repo/todo.c")
```

目标 Agent 如果关注了 `AGENT_WATCH_MESSAGE`，内核会设置 `AGENT_EVENT_MESSAGE` 并唤醒目标进程。

## 10. 文件修改事件

文件修改事件由 AgentFS 和 patch 工具触发。

```text
agent_watch_file("repo/todo.c")
```

当 `patch_file` 修改该文件后，内核触发：

```text
AGENT_EVENT_FILEMOD
```

CodeLab 中 Reviewer-Agent 使用该事件感知 `repo/todo.c` 被修改。

## 11. 测试程序

### 11.1 agenttest

运行：

```text
agenttest
```

覆盖：

```text
agent_create
agent_info
context_push/query/rollback/clear
tool_call
AgentFS 属性设置和 query_file
```

### 11.2 agentlooptest

运行：

```text
agentlooptest
```

覆盖：

```text
heartbeat
agent_wait
MESSAGE 唤醒
FILEMOD 唤醒
多 Agent 调度
```

### 11.3 agentfsbench

运行：

```text
agentfsbench
```

覆盖：

```text
AgentFS 索引查询
全量遍历查询
索引查询与遍历查询性能对比
```

### 11.4 agentperftest

运行：

```text
agentperftest
```

覆盖：

```text
Context 操作开销
Tool Call 开销
AgentFS 查询开销
Agent Loop 等待/唤醒开销
```

### 11.5 agentinnovationtest

运行：

```text
agentinnovationtest
```

覆盖：

```text
Shared Query Cache
事件感知调度
动态工具注册
```

## 12. CodeLab Demo 运行

CodeLab 预置仓库：

```text
repo/main.c
repo/todo.c
repo/todo.h
repo/test.c
repo/README
```

运行规则版：

```text
planner_agent
```

运行 LLM demo 版：

```text
planner_agent llm-demo
```

运行真实 API 版需要使用宿主机 driver：

```bash
export AGENTOS_LLM_API_KEY="你的第三方 API key"
export AGENTOS_LLM_MODEL="你的模型名称"
export AGENTOS_LLM_API_URL="第三方 chat completions 接口地址"
python3 tools/llm_qemu_driver.py --mode api
```

成功输出包含：

```text
[Planner-Agent] task: fix todo delete bug
[Kernel-AgentLoop] HEARTBEAT -> wakeup Planner-Agent
[Kernel-FS] query_file type=code,module=todo,keyword=delete -> cache_hit=0, used_index=1
[Kernel-FS] reviewer repeated query -> shared query cache hit
[Patch-Agent] patch_file repo/todo.c replace BUG with task_count--
[Kernel-FS] FILEMOD repo/todo.c -> wakeup Reviewer-Agent
[Test-Agent] tool_call run_rule_test_dyn -> {status=ok,...passed=3,total=3}
[Reviewer-Agent] diff_file + rule result -> approve
[Summary] dynamic tool: run_rule_test_dyn registered and used by Test-Agent
[Agent-Loop] final loop_state=5
```

## 13. LLM Bridge 运行方式

自动 QEMU demo 模式：

```bash
python3 tools/llm_qemu_driver.py --mode demo
```

自动 QEMU API 模式：

```bash
export AGENTOS_LLM_API_KEY="你的第三方 API key"
export AGENTOS_LLM_MODEL="你的模型名称"
export AGENTOS_LLM_API_URL="第三方 chat completions 接口地址"
python3 tools/llm_qemu_driver.py --mode api
```

协议格式：

```text
@@AGENTOS_LLM_REQ id=planner role=planner prompt=fix todo delete bug @@END
@@AGENTOS_LLM_RESP id=planner state=done text=... action=start_codelab @@END
```

LLM 返回 `action=start_codelab` 时，`planner_agent llm-api` 执行 CodeLab 修复流程。返回其他 action 时，Planner 不启动修复链路。

## 14. 真实 API 环境变量

| 环境变量 | 作用 |
| --- | --- |
| `AGENTOS_LLM_API_KEY` | 第三方模型 API key |
| `AGENTOS_LLM_MODEL` | 模型名称 |
| `AGENTOS_LLM_API_URL` | chat completions 兼容接口地址 |

API key 只保存在宿主机环境变量中，不写入 xv6 镜像，也不进入内核。

## 15. 演示对应关系

| 场景输出 | 对应 AgentOS 功能 |
| --- | --- |
| `HEARTBEAT -> wakeup Planner-Agent` | Agent Loop 心跳唤醒 |
| `query_file ... cache_hit=0` | AgentFS 索引查询 |
| `shared query cache hit` | 跨 Agent 共享查询缓存 |
| `patch_file repo/todo.c` | Tool Call 文件修改 |
| `FILEMOD repo/todo.c` | 文件修改事件 |
| `run_rule_test_dyn` | 动态工具注册 |
| `sched=8/8` 等输出 | 事件感知调度参数 |
| `final loop_state=5` | Agent Loop DONE 生命周期 |
