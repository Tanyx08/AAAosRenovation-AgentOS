# AgentOS 接口与运行说明

本文档面向 AgentOS 的使用、测试和答辩演示，说明当前仓库实际提供的用户态接口、Tool Router、AgentFS、Agent Loop、CodeLab 与量化测试入口。接口声明以 `user/user.h` 为准，ABI 结构和常量以 `kernel/agent.h` 为准。

## 1. 编译与启动

在 `AgentOS-AAA` 目录执行：

```bash
make clean
make -j2
make qemu
```

`make` 会构建内核、用户程序和 `fs.img`，通常不需要单独执行 `make fs.img`。退出 QEMU 使用 `Ctrl-a x`。

需要 GDB 调试时分别在两个终端执行：

```bash
make qemu-gdb
riscv64-unknown-elf-gdb
```

## 2. 编写 Agent 程序

用户程序通常包含：

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/agent.h"
#include "user/user.h"
```

最小示例：

```c
struct agent_info info;

if((uint64)agent_create(AGENT_TYPE_WORKER, 10, 1024) == 0)
  exit(1);
if(agent_role_set(AGENT_ROLE_RETRIEVER) < 0)
  exit(1);
agent_info(&info);
```

`agent_create(type, heartbeat_interval, resource_quota)` 的参数含义是：

| 参数 | 含义 |
| --- | --- |
| `type` | `AGENT_TYPE_PRIMARY` 或 `AGENT_TYPE_WORKER` |
| `heartbeat_interval` | 心跳周期，0 表示创建时不启用心跳 |
| `resource_quota` | Context 可用配额，单位为字节；不能超过固定的 8192 字节 Context 区 |

返回值是用户态只读 Context 区的起始地址；失败返回 0。这里的 quota 不是页数，也不是调度时间片。调度配额由 `agent_sched_set` 单独设置。

## 3. Agent 类型、角色与 capability

### 3.1 类型

| 类型 | 定位 |
| --- | --- |
| `AGENT_TYPE_PRIMARY` | 工作流负责人，创建时拥有全部 capability |
| `AGENT_TYPE_WORKER` | 执行具体子任务，创建时只有基础 capability |
| `AGENT_TYPE_NORMAL` | 普通进程，不具有 Agent 身份 |

### 3.2 角色

Worker 可通过 `agent_role_set(role)` 一次性确定职责：

```c
AGENT_ROLE_PLANNER
AGENT_ROLE_RETRIEVER
AGENT_ROLE_PATCH
AGENT_ROLE_TEST
AGENT_ROLE_REVIEWER
AGENT_ROLE_TOOL_SERVICE
```

角色会分配对应的默认 capability，并在设置后锁定，防止进程反复换角色提权。CodeLab 中 `planner_agent` 是 Primary，在业务上承担 Planner；其余 Retriever、Patch、Test、Reviewer 和 Tool-Service 都是 Worker。

### 3.3 capability

每个 Agent 的 PCB 保存一个 `uint64` 位掩码，主要权限包括：

```c
AGENT_CAP_QUERY_PROCESS
AGENT_CAP_QUERY_FILE
AGENT_CAP_READ_FILE
AGENT_CAP_PATCH_FILE
AGENT_CAP_SEND_MESSAGE
AGENT_CAP_WATCH_FILE
AGENT_CAP_REGISTER_TOOL
AGENT_CAP_SCHED_CONFIG
AGENT_CAP_LEASE_ACQUIRE
AGENT_CAP_WORKFLOW_CTRL
AGENT_CAP_AUDIT_READ
```

Tool Router 在执行工具前检查调用者是否具有工具要求的 capability。`agent_cap_set(caps)` 只能删减当前权限，不能增加原本没有的权限；非法提权会被拒绝并写入审计记录。`fork` 后 capability 清零，避免权限随子进程扩散。

## 4. 用户态接口总览

### 4.1 Agent 基础与调度

| 接口 | 作用 |
| --- | --- |
| `agent_create(type, heartbeat, quota)` | 将当前进程注册为 Agent，返回 Context 地址 |
| `agent_info(info)` | 读取类型、Context、调度、capability 和邮箱统计 |
| `agent_role_set(role)` | 一次性设置角色及默认 capability |
| `agent_cap_set(caps)` | 主动削减 capability |
| `agent_priority_set(priority)` | 设置旧版 Agent 优先级字段 |
| `agent_sched_set(priority, quota)` | 设置 Agent 感知调度优先级和预算 |
| `agent_query_agent(role, capability, group, buf, len)` | 按角色、能力和组发现 Agent |
| `agent_cascade_kill(reason)` | 级联终止当前工作流中的 Worker |

`agent_query_agent` 中筛选值为负数时表示不限制该字段。

### 4.2 Context Path

| 接口 | 作用 |
| --- | --- |
| `context_push(node)` | 由系统调用追加 Context 节点 |
| `context_query(buf, len)` | 将 Context Path 复制到用户缓冲区 |
| `context_rollback(keep_nodes)` | 仅保留最前面的 `keep_nodes` 个节点 |
| `context_clear()` | 清空当前 Context |
| `context_validate(sequence, result)` | 检查节点依赖文件是否仍为原版本 |
| `agent_context_verify()` | 校验内核维护的 Context 摘要链 |

Context 节点包含 `sequence`、`request_id`、`span_id`、`cause_sequence`、状态、请求和结果。内核维护固定 ABI Header、单调序列号和可信摘要；用户映射区为只读，正常修改必须经过系统调用。

`context_validate` 的结果状态包括：

```c
AGENT_CONTEXT_VALID
AGENT_CONTEXT_STALE
AGENT_CONTEXT_NO_DEP
AGENT_CONTEXT_NOT_FOUND
AGENT_CONTEXT_DELETED
```

AgentFS 的重复查询还可以命中跨 Agent 共享 Query Cache；文件版本变化后缓存失效。

### 4.3 Tool Router

| 接口 | 作用 |
| --- | --- |
| `tool_call(request, response)` | 调用单个工具 |
| `tool_call_batch(batch)` | 一次提交最多 8 个工具调用 |
| `tool_list(buf, len)` | 返回带参数、权限和结果说明的工具列表 |
| `tool_schema(name, schema)` | 查询某个工具的 Schema 与权限要求 |

统一请求结构：

```c
struct agent_tool_request req;
struct agent_tool_response resp;

strcpy(req.tool, "read_file");
strcpy(req.params, "path=repo/todo.c");
tool_call(&req, &resp);
```

工具参数统一使用：

```text
key=value;key=value
```

Tool Router 负责解析参数、查找工具、capability 与组权限检查、执行、结果格式化、指标统计和必要的审计记录。

### 4.4 内置工具

| 工具 | 主要参数 | 所需 capability |
| --- | --- | --- |
| `get_system_status` | 无 | `QUERY_PROCESS` |
| `query_process` | `type`（可选） | `QUERY_PROCESS` |
| `send_message` | `target_pid;message` | `SEND_MESSAGE` |
| `read_context` | 无 | 无 |
| `read_file` | `path` | `READ_FILE` |
| `patch_file` | `path;op;old;new` | `PATCH_FILE` |
| `run_rule_test` | `target` | `QUERY_FILE` |
| `diff_file` | `path` | `READ_FILE` |
| `set_file_attr` | `path;key;value` | `PATCH_FILE` |
| `get_file_attr` | `path;key` | `QUERY_FILE` |
| `del_file_attr` | `path;key` | `PATCH_FILE` |
| `query_file` | `type;owner;tags;keyword;public;mode` | `QUERY_FILE` |
| `query_agent` | `role;capability;group` | `QUERY_PROCESS` |
| `get_workflow_metrics` | 无 | `AUDIT_READ` |
| `lease_begin` | `path` | `LEASE_ACQUIRE` |
| `lease_commit` | `lease_id;expected_version` | `LEASE_ACQUIRE` |
| `lease_abort` | `lease_id` | `LEASE_ACQUIRE` |

实际可用参数可在运行时通过 `tool_list` 或 `tool_schema` 查询。

### 4.5 动态工具

| 接口 | 作用 |
| --- | --- |
| `tool_register(name, flags)` | Tool-Service 注册动态工具 |
| `tool_recv(request)` | 阻塞接收动态工具请求 |
| `tool_reply(id, result, status)` | 返回执行结果并唤醒调用者 |

注册动态工具要求 `AGENT_CAP_REGISTER_TOOL`。`AGENT_TOOL_FLAG_PUBLIC` 表示其他组也可发现和调用；非公开工具仅限同组。动态请求槽有界，服务退出、请求超时或槽位耗尽时返回明确错误。

CodeLab 使用的动态工具是 `run_rule_test_dyn`，实现位于 `user/rule_test_tool_agent.c`。

### 4.6 文件编辑 Lease

| 接口 | 作用 |
| --- | --- |
| `agent_lease_begin(path, result)` | 获取文件写 Lease，返回 lease ID 和基础版本 |
| `agent_lease_commit(id, expected_version)` | 按预期版本提交 |
| `agent_lease_abort(id)` | 主动释放 Lease |

Lease 用于避免多个 Patch Agent 同时修改同一文件。Lease 有持有者、基础版本和到期时间；版本不一致时提交失败，从而防止覆盖更新。对应测试程序为 `agentlease`。

## 5. Agent Loop、事件与邮箱

### 5.1 接口

| 接口 | 作用 |
| --- | --- |
| `agent_heartbeat_set(interval)` | 设置心跳周期 |
| `agent_heartbeat_stop()` | 停止心跳 |
| `agent_watch(mask)` | 订阅事件 |
| `agent_unwatch(mask)` | 取消订阅 |
| `agent_watch_file(path)` | 关注指定文件 |
| `agent_wait(loop_state, event)` | 阻塞等待事件 |
| `agent_wait_timeout(loop_state, ticks, event)` | 带超时等待，可避免无限阻塞 |

事件包括：

```c
AGENT_EVENT_HEARTBEAT
AGENT_EVENT_MESSAGE
AGENT_EVENT_FILEMOD
AGENT_EVENT_PARENT_GONE
```

等待事件会返回来源 PID、消息类型、长度、序列号、request ID、消息内容或文件路径。

### 5.2 FIFO 邮箱

每个 Agent 的 PCB 保存容量为 8 的有界 FIFO 邮箱，其中为系统消息预留 2 个槽位。消息包含发送者 PID、类型、长度、序列号、request ID 和 payload。消息按入队顺序消费；邮箱满时返回忙或记录丢弃统计，不会像单消息槽那样静默覆盖前一条消息。

消息通过工具发送：

```text
tool=send_message
params=target_pid=4;message=role=test;target=todo_delete
```

订阅 `AGENT_WATCH_MESSAGE` 的目标会被唤醒。`patch_file` 修改被关注文件时会产生 `AGENT_EVENT_FILEMOD`，CodeLab 的 Reviewer 依靠该事件启动审查。

## 6. AgentFS

AgentFS 将文件内容、inode 扩展属性、索引查询、共享缓存和文件版本统一暴露给 Tool Router。

常用调用示例：

```text
set_file_attr: path=repo/todo.c;key=type;value=code
query_file:    type=code;keyword=delete;public=true
read_file:     path=repo/todo.c
patch_file:    path=repo/todo.c;op=replace;old=BUG;new=task_count--
diff_file:     path=repo/todo.c
```

`query_file` 结果包含匹配路径及 `cache_hit`、`used_index`、`scanned` 等字段，可用于说明索引和共享缓存是否生效。实现主要位于 `kernel/agent_fs.c`，路由位于 `kernel/agent_tool.c`。

## 7. 功能测试与量化测试

进入 xv6 shell 后运行：

| 命令 | 验证内容 |
| --- | --- |
| `agenttest` | Agent、Context、Tool、权限基础功能 |
| `agentlooptest` | 心跳、消息、文件事件、等待和调度 |
| `agentfsbench` | AgentFS 索引和遍历查询 |
| `agentperftest` | 查询、等待、唤醒和调度性能 |
| `agentinnov` | 共享缓存、动态工具、Schema、batch、权限等创新功能 |
| `agentlease` | Lease 冲突、版本和释放 |
| `agentorphan` | 父进程退出、孤儿 Agent 和级联清理 |
| `mailbench` | FIFO 顺序、容量、并发发送和丢弃统计 |
| `codelabfail` | 动态工具服务退出等失败恢复场景 |

量化实验程序：

| 命令 | 主要指标 |
| --- | --- |
| `agentfsmetric` | scan/index/cache 的 ticks、扫描数和命中数 |
| `contextmetric` | 首次查询、复用、失效后的调用数和节省 ticks |
| `ctxvermetric` | 文件修改前后 Context 版本有效性 |
| `waitmetric` | 阻塞等待与轮询的延迟、循环数、CPU ticks |
| `schedmetric` | dispatch、p50/p95/max wait、公平性 |
| `mailbench` | accepted/received/busy、顺序错误和重复数 |

程序使用 `[TEST]`、`[METRIC]` 和 `[SUMMARY]` 输出结构化结果，便于宿主机脚本提取并生成答辩图表。

## 8. CodeLab 多 Agent 场景

直接运行规则版：

```text
planner_agent
```

运行内置 LLM 协议演示版：

```text
planner_agent llm-demo
```

场景结构：

```text
Planner-Agent（Primary）
├── Retriever-Agent（查询、读取、共享查询复用）
├── Patch-Agent（Lease、修改文件）
├── Test-Agent（调用动态测试工具）
├── Reviewer-Agent（FILEMOD 唤醒、diff 审查）
└── Tool-Service（注册并执行 run_rule_test_dyn）
```

预置目标文件位于 `repo/`，流程为：任务规划 → 定位代码 → 获取 Lease 并修改 → 动态工具测试 → 文件事件唤醒审查 → 汇总工作流指标。

## 9. LLM Bridge

宿主机自动运行 demo：

```bash
python3 tools/llm_qemu_driver.py --mode demo
```

调用兼容 Chat Completions 的真实 API：

```bash
export AGENTOS_LLM_API_KEY="你的 API key"
export AGENTOS_LLM_MODEL="模型名称"
export AGENTOS_LLM_API_URL="接口地址"
python3 tools/llm_qemu_driver.py --mode api
```

串口协议：

```text
@@AGENTOS_LLM_REQ id=planner role=planner prompt=fix todo delete bug @@END
@@AGENTOS_LLM_RESP id=planner state=done text=... action=start_codelab @@END
```

只有响应包含 `action=start_codelab` 时，Planner 才继续执行修复链。API key 仅存在宿主机环境变量中，不进入 xv6 镜像或内核。

## 10. 代码定位

| 模块 | 主要文件 |
| --- | --- |
| ABI、常量、结构体 | `kernel/agent.h` |
| PCB Agent 字段 | `kernel/proc.h` |
| 生命周期、角色、权限、邮箱 | `kernel/agent.c` |
| Context Path | `kernel/agent_context.c` |
| Tool Router、动态工具 | `kernel/agent_tool.c` |
| AgentFS | `kernel/agent_fs.c` |
| Agent Loop | `kernel/agent_loop.c` |
| 系统调用桥接 | `kernel/sysagent.c` |
| 系统调用注册 | `kernel/syscall.c`、`kernel/syscall.h` |
| 用户态声明与桩 | `user/user.h`、`user/usys.pl` |
| CodeLab 总控 | `user/planner_agent.c` |
| LLM 宿主机驱动 | `tools/llm_qemu_driver.py` |

阅读一条完整调用链时，建议按以下顺序：

```text
用户程序 → user/user.h → user/usys.pl → kernel/syscall.c
        → kernel/sysagent.c → kernel/agent*.c → PCB/AgentFS
```

## 11. 常见注意事项

- `agent_create` 的第三个参数是字节配额，`1024` 可以运行，但只允许使用 1 KiB Context；需要完整 Context 区时传 `8192`。
- `context_rollback(n)` 表示保留 `n` 个节点，不是删除 `n` 个节点。
- 工具参数使用分号分隔的 `key=value`，不要混用逗号。
- 动态工具名不能与内置工具重名，注册者需要 Tool-Service 对应权限。
- 文件写操作应先取得 Lease，并在提交时检查版本。
- 可能长期等待的代码优先使用 `agent_wait_timeout`，并处理 TIMEOUT、CANCELLED 和 NO_SOURCE。
- xv6 文件名最长 14 字符，因此部分测试在镜像中使用 `agentinnov`、`agentlease`、`mailbench` 等短名称。
