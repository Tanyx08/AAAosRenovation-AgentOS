# AgentOS on xv6

本仓库基于 xv6-riscv 实现 AgentOS：在教学操作系统内核中加入 Agent 进程模型、Agent Context、Tool Call、AgentFS 查询扩展、Agent Loop 事件机制、事件感知调度，以及 CodeLab 多 Agent 协作演示。

项目目标是让用户态 Agent 不再只是普通进程，而是能被内核识别、调度、唤醒，并通过统一接口访问工具、文件语义索引和上下文路径。

## 选题:proj61 面向AI智能体的操作系统内核（Agent-OS）

总体要求：在一个教学操作系统内核（如uCore 、rCore等）上，设计并实现面向AI智能体的内核功能模块（Agent-OS）。最终交付物为一套可在QEMU上运行的完整系统，包含内核代码、用户态测试程序和演示场景。

具体赛题要求请看文档**proj61赛题说明.md**

## 功能总览

| 模块 | 已实现能力 |
| --- | --- |
| Agent 进程模型 | `agent_create` 标记 Agent，PCB 保存 Agent 类型、group、Context、事件、调度状态 |
| Agent Context | `context_push/query/rollback/clear` 记录 Agent 的工具调用和观察路径 |
| Tool Call | 内置工具调用，支持消息发送、进程查询、文件查询、读写和 diff |
| 动态工具注册 | 用户态工具服务可运行时注册工具，其他 Agent 通过 `tool_call` 调用 |
| AgentFS | inode 元数据、属性查询、内容摘要、倒排索引、结构化结果 |
| Shared Query Cache | 多 Agent 对相同查询复用缓存结果，避免重复扫描 |
| Agent Loop | 心跳、消息事件、文件修改事件、`agent_wait` 睡眠/唤醒 |
| 事件感知调度 | priority/quota + pending event + aging 的调度评分 |
| CodeLab 场景 | 多 Agent 协作修复 `/repo/todo.c` 的 delete bug |
| LLM Bridge | xv6 用户态 bridge + 宿主机 proxy，可接 demo 模型或第三方 API |

## 代码结构

```text
kernel/agent.h
  AgentOS ABI、常量、结构体和内核函数声明

kernel/agent.c
  Agent 核心创建、查询和继承逻辑

kernel/agent_context.c
  Agent Context Path 管理

kernel/agent_tool.c
  Tool Router、内置工具和动态工具注册

kernel/agent_fs.c
  AgentFS 属性系统、索引查询和共享查询缓存

kernel/agent_loop.c
  心跳、事件等待、文件修改事件和调度评分

kernel/sysagent.c
  Agent 系统调用入口

kernel/proc.c, kernel/proc.h, kernel/trap.c
  Agent PCB 字段、进程生命周期、scheduler 和 tick 接入

user/agenttest.c
user/agentlooptest.c
user/agentfsbench.c
user/agentperftest.c
user/agentinnovationtest.c
  功能测试、性能测试和创新点测试

user/planner_agent.c
user/retriever_agent.c
user/patch_agent.c
user/test_agent.c
user/reviewer_agent.c
user/rule_test_tool_agent.c
  Task 6 CodeLab 多 Agent 协作演示

user/llm_bridge.c
tools/llm_qemu_driver.py
tools/llm_proxy.py
  LLM bridge、QEMU driver 和宿主机 proxy
```

## 编译运行

在宿主机执行：

```bash
make clean
make
make fs.img
make qemu
```

进入 xv6 shell 后运行测试：

```text
agenttest
agentlooptest
agentfsbench
agentperftest
agentinnovationtest
```

基础测试期望结果：

```text
agenttest: all tests passed
agentlooptest: all tests passed
agentfsbench: all tests passed
agentinnovationtest: all tests passed
```

## CodeLab 演示

CodeLab 场景中，系统预置了一个小型代码仓库：

```text
repo/main.c
repo/todo.c
repo/todo.h
repo/test.c
repo/README
```

运行规则 demo 版：

```text
planner_agent
```

运行 LLM demo 版：

```text
planner_agent llm-demo
```

演示链路：

```text
Planner-Agent 被 HEARTBEAT 唤醒
  -> 创建 Retriever / Patch / Test / Reviewer / Tool-Service
  -> Retriever 通过 AgentFS 查询 repo/todo.c
  -> Reviewer 重复查询命中 shared query cache
  -> Patch 修改 repo/todo.c
  -> FILEMOD 唤醒 Reviewer
  -> Test 调用动态工具 run_rule_test_dyn
  -> Reviewer approve
  -> Planner 输出 summary 并进入 DONE
```

关键输出示例：

```text
[Kernel-FS] query_file type=code,module=todo,keyword=delete -> cache_hit=0, used_index=1
[Kernel-FS] reviewer repeated query -> shared query cache hit
[Tool-Service] register run_rule_test_dyn ok
[Test-Agent] tool_call run_rule_test_dyn -> {status=ok,...passed=3,total=3}
[Reviewer-Agent] diff_file + rule result -> approve
[Agent-Loop] final loop_state=5
```

## LLM API 演示

真实 LLM 接入采用“xv6 用户态 bridge + 宿主机 proxy”的结构。API key、模型名和网络请求只保留在宿主机，xv6 内部只收发压缩后的串口协议。

运行 demo 模式：

```bash
python3 tools/llm_qemu_driver.py --mode demo
```

运行真实第三方 API 模式：

```bash
export AGENTOS_LLM_API_KEY="你的第三方 API key"
export AGENTOS_LLM_MODEL="你的模型名称"
export AGENTOS_LLM_API_URL="第三方 chat completions 接口地址"
python3 tools/llm_qemu_driver.py --mode api
```

串口协议格式：

```text
@@AGENTOS_LLM_REQ id=planner role=planner prompt=fix todo delete bug @@END
@@AGENTOS_LLM_RESP id=planner state=done text=... action=start_codelab @@END
```

LLM 返回 `action=start_codelab` 后，`planner_agent llm-api` 执行同一套 CodeLab 多 Agent 修复流程。

## 场景功能与 AgentOS 功能对应表

| 场景功能 | AgentOS 功能 |
| --- | --- |
| Planner-Agent 启动并规划修复流程 | Agent 进程创建、Agent Loop 心跳唤醒 |
| Retriever-Agent 按语义查找 `todo.c` | AgentFS 属性查询、内容摘要索引、结构化查询结果 |
| 多个 Agent 重复查询同一仓库 | Shared Query Cache，共享查询结果缓存 |
| Patch-Agent 修改 `repo/todo.c` | Tool Call 文件操作、文件修改事件触发 |
| Reviewer-Agent 被文件修改唤醒 | FILEMOD 事件监听、事件驱动 Agent Loop |
| Test-Agent 调用 `run_rule_test_dyn` | 动态工具注册、Tool Router 请求转发 |
| 不同角色设置不同优先级 | 事件感知调度、priority/quota 调度参数 |
| Summary 输出完整执行链路 | Context Path 记录、Loop DONE 生命周期管理 |
| `planner_agent llm-demo/llm-api` | LLM Bridge、宿主机 Proxy、串口请求/响应协议 |

## 主要文档

| 类别 | 文档 | 说明 |
| --- | --- | --- |
| 赛题要求 | [proj61赛题说明.md](proj61赛题说明.md) | 原始比赛题目和任务要求 |
| 总体实现 | [AGENT_OS_IMPLEMENTATION.md](AGENT_OS_IMPLEMENTATION.md) | 目标、题目分析、系统框架、模块实现、测试情况和文件说明 |
| 运行指南 | [AGENT_OS_GUIDE.md](AGENT_OS_GUIDE.md) | 编译运行、系统调用接口、测试程序、CodeLab 和 LLM bridge 使用方式 |
| 创新功能 | [AGENT_OS_创新功能.md](AGENT_OS_创新功能.md) | Shared Query Cache、事件感知调度、动态工具注册 |
| 性能测试 | [AGENT_PERFORMANCE_TESTS.md](AGENT_PERFORMANCE_TESTS.md) | AgentOS 性能测试点、运行方式和结果说明 |
| 综合场景 | [task6-CodeLab场景说明.md](task6-CodeLab场景说明.md) | CodeLab 多 Agent 修复代码仓库场景 |
| 接口速查 | [agent功能接口.md](agent功能接口.md) | AgentOS 功能接口和 syscall 速查 |

## xv6 基础说明

本项目基于 xv6-riscv。原始 xv6 是 Dennis Ritchie 和 Ken Thompson 的 Unix Version 6 的教学操作系统重实现，RISC-V 版本由 MIT 6.1810 课程维护。本仓库在 xv6 基础上增加 AgentOS 相关内核扩展和用户态演示程序。
