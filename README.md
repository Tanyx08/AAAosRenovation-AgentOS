# AgentOS on xv6

本仓库基于 xv6-riscv 实现 AgentOS：在教学操作系统内核中加入 Agent 进程模型、只读可信 Context、统一 Tool Router、AgentFS 查询扩展、Agent Loop、事件感知调度、capability 权限、FIFO 邮箱、文件 Lease，以及 CodeLab 多 Agent 协作演示。

项目目标是让用户态 Agent 不再只是普通进程，而是能被内核识别、调度、唤醒，并通过统一接口访问工具、文件语义索引和上下文路径。

## 选题:proj61 面向AI智能体的操作系统内核（Agent-OS）

总体要求：在一个教学操作系统内核（如uCore 、rCore等）上，设计并实现面向AI智能体的内核功能模块（Agent-OS）。最终交付物为一套可在QEMU上运行的完整系统，包含内核代码、用户态测试程序和演示场景。

具体赛题要求请看文档[proj61赛题说明.md](proj61赛题说明.md)

设计文档请看[设计文档.docx](设计文档.docx) 

进展汇报幻灯片请看[项目PPT.pptx](项目PPT.pptx)

演示视频链接：
通过百度网盘分享的文件：AAAos翻新队...
链接：https://pan.baidu.com/s/1xDvfA2AOVBsstNnwEhupEA 
提取码：qZx4 
复制这段内容打开「百度网盘APP 即可获取」

## 主要文档

| 类别 | 文档 | 说明 |
| --- | --- | --- |
| 赛题要求 | [proj61赛题说明.md](proj61赛题说明.md) | 原始比赛题目和任务要求 |
| 设计文档 | [决赛设计文档.pdf](决赛设计文档.pdf) | 设计文档 |
| 项目PPT | [项目PPT.pptx](项目PPT.pptx) | 进展汇报幻灯片 |
| 运行指南 | [AgentOS 接口与运行说明.md](AgentOS%20接口与运行说明.md) | 编译运行、系统调用接口、测试程序、CodeLab 和 LLM bridge 使用方式 |
| 性能测试 | [AgentOS 性能测试说明.md](AgentOS%20性能测试说明.md) | AgentOS 性能测试点、运行方式和结果说明 |

## 功能总览

### 任务完成情况

| 赛题任务 | 完成情况 | 当前实现 | 验证测试 | 主要实现代码 |
| --- | --- | --- | --- | --- |
| 任务一：Agent 进程创建与地址空间设计 | 已完成 | PCB 扩展；`agent_create/agent_info`；固定 ABI、用户态只读 Context 区；普通进程与 Agent 共存 | `user/agenttest.c`、`user/agentlooptest.c` | `kernel/agent.c`、`kernel/proc.h`、`kernel/proc.c`、`kernel/vm.c`、`kernel/sysagent.c` |
| 任务二：内核结构化交互接口 | 已完成 | 统一 `key=value` 协议、Tool Router、结构化响应、错误码、工具列表及十余个内置工具 | `user/agenttest.c`、`user/agentinnovationtest.c` | `kernel/agent_tool.c`、`kernel/sysagent.c`、`kernel/agent.h` |
| 任务三：上下文路径管理 | 已完成 | push/query/rollback/clear、内核配额、FIFO 淘汰、固定 Header、序列号和摘要校验 | `user/agenttest.c`、`user/contextmetric.c`、`user/ctxvermetric.c` | `kernel/agent_context.c`、`kernel/agent.c`、`kernel/agent.h` |
| 任务四：面向 Agent 的文件系统扩展 | 已完成 | inode 键值属性、内容摘要、属性索引、多条件查询、结构化结果和共享 Query Cache | `user/agentfsbench.c`、`user/agentfsmetric.c`、`user/contextmetric.c` | `kernel/agent_fs.c`、`kernel/agent_tool.c`、`kernel/fs.c`、`kernel/fs.h` |
| 任务五：Agent Loop 内核运行机制 | 已完成 | 可调心跳、消息/文件事件、阻塞与超时等待、生命周期管理、priority/quota/aging 调度 | `user/agentlooptest.c`、`user/agentperftest.c`、`user/waitmetric.c`、`user/schedmetric.c` | `kernel/agent_loop.c`、`kernel/proc.c`、`kernel/trap.c`、`kernel/agent.c` |
| 任务六：综合场景与自由创新 | 已完成 | CodeLab 整合任务一至五；多 Agent 查询、修复、测试、审查；量化实验与 LLM Bridge | `user/planner_agent.c` 场景、`user/codelabfail.c` | `user/*_agent.c`、`kernel/workflow.c`、`tools/llm_qemu_driver.py` |

### 创新功能

| 创新功能 | 完成情况 | 简介 | 验证测试 | 主要实现代码 |
| --- | --- | --- | --- | --- |
| 跨 Agent 共享 Query Cache | 已完成 | 相同查询跨进程复用；文件版本变化后自动失效 | `user/agentinnovationtest.c`、`user/contextmetric.c`、`user/agentfsmetric.c` | `kernel/agent_fs.c` |
| 动态工具注册 | 已完成 | Tool-Service 运行时注册，Router 转发请求并同步返回结果 | `user/agentinnovationtest.c`、`user/codelabfail.c`、CodeLab | `kernel/agent_tool.c`、`user/rule_test_tool_agent.c` |
| capability、角色与 Agent 发现 | 已完成 | 角色分配最小权限；统一鉴权；只能降权；按角色/能力/组查询 Agent | `user/agenttest.c`、`user/agentinnovationtest.c` | `kernel/agent.c`、`kernel/agent_tool.c`、`kernel/proc.h` |
| 有界 FIFO 邮箱 | 已完成 | 记录来源、类型、长度、序列号；系统槽位预留；顺序消费和丢弃统计 | `user/agentlooptest.c`、`user/agentmailbench.c` | `kernel/agent.c`、`kernel/agent.h` |
| 文件编辑 Lease | 已完成 | 写租约、基础版本、超时回收和提交校验，防止覆盖更新 | `user/agentleasetest.c` | `kernel/agent.c`、`kernel/agent_tool.c` |
| 可信 Context 与版本验证 | 已完成 | 固定 ABI、只读映射、摘要链和文件依赖版本校验 | `user/agenttest.c`、`user/ctxvermetric.c` | `kernel/agent_context.c`、`kernel/vm.c` |
| Context 复用与因果字段 | 已完成 | 记录 request/span/cause，重复查询展示缓存命中并统计节省开销 | `user/contextmetric.c`、CodeLab | `kernel/agent_context.c`、`kernel/workflow.c` |
| Tool Schema 与批量调用 | 已完成 | 运行时查询参数、结果和权限；单次批量执行最多 8 个请求 | `user/agentinnovationtest.c` | `kernel/agent_tool.c`、`kernel/sysagent.c` |
| 超时等待与级联清理 | 已完成 | timeout/cancel/no-source；Primary 退出或失败时清理 Worker 和动态资源 | `user/agentinnovationtest.c`、`user/agentorphan.c`、`user/codelabfail.c` | `kernel/agent_loop.c`、`kernel/agent.c`、`kernel/workflow.c` |
| 事件感知调度与心跳时间轮 | 已完成 | 结合事件、priority、quota、aging 和预算补充；时间轮减少心跳扫描 | `user/agentperftest.c`、`user/waitmetric.c`、`user/schedmetric.c` | `kernel/agent_loop.c`、`kernel/proc.c`、`kernel/trap.c` |
| 结构化指标与故障实验 | 已完成 | 输出延迟、扫描量、缓存命中、公平性和邮箱可靠性指标 | `user/*metric.c`、`user/agentmailbench.c`、`user/codelabfail.c` | `kernel/workflow.c`、各测试程序 |
| 外部 LLM Bridge | 已完成 | 串口协议连接宿主机代理，API key 和网络访问不进入内核 | `planner_agent llm-demo`、`tools/llm_qemu_driver.py --mode demo` | `user/planner_agent.c`、`user/llm_bridge.c`、`tools/llm_qemu_driver.py` |

## 项目目录索引

```text
AgentOS-AAA/
├── kernel/                 xv6 内核及 AgentOS 内核模块
├── user/                   用户程序、Agent 程序和功能/量化测试
├── tools/                  LLM Bridge 驱动、自动演示和实验脚本
│   └── perf/               指标提取、双系统对比和结果校验脚本
├── repo/                   CodeLab 使用的示例代码仓库
├── baseline-xv6/           用于量化对比的原生 xv6 基线系统
├── mkfs/                   xv6 文件系统镜像生成工具
├── conf/                   实验构建配置
├── docs/                   项目辅助文档
├── tests-result/           测试运行结果（生成目录）
├── dual-results/           AgentOS 与 xv6 对比结果（生成目录）
├── Makefile                内核、用户程序和 fs.img 构建入口
├── README.md               项目总览与运行入口
├── proj61赛题说明.md       比赛原始任务说明
├── AgentOS 接口与运行说明.md 详细接口与使用说明
├── 设计文档.docx           项目设计文档
└── 项目PPT.pptx            项目汇报材料
```

其中 `fs.img`、`kernel/kernel`、`*.o`、`*.asm` 和 `*.sym` 等均为构建生成物，不属于核心源码。阅读 AgentOS 实现建议从 `kernel/agent.h` 开始，再依次阅读 `agent.c`、`agent_context.c`、`agent_tool.c`、`agent_fs.c` 和 `agent_loop.c`。

## 代码结构

### 内核实现

| 文件 | 作用 |
| --- | --- |
| `kernel/agent.h` | AgentOS ABI、常量、结构体、错误码和内核函数声明 |
| `kernel/proc.h` | PCB 中的 Agent 类型、角色、Context、邮箱、调度、工作流和 capability 字段 |
| `kernel/agent.c` | Agent 创建、角色、权限、发现、FIFO 邮箱、Lease、审计和级联清理 |
| `kernel/agent_context.c` | Context Header、节点、配额淘汰、摘要链、回滚和文件版本验证 |
| `kernel/agent_tool.c` | Tool Router、内置工具、Schema、batch、动态工具注册与请求槽 |
| `kernel/agent_fs.c` | inode 属性、内容摘要、属性索引、结构化查询和共享 Query Cache |
| `kernel/agent_loop.c` | 心跳时间轮、事件等待、超时/取消和调度评分 |
| `kernel/workflow.c` | 工作流成员、状态和量化指标聚合 |
| `kernel/sysagent.c` | Agent 系统调用参数复制和内核实现桥接 |
| `kernel/syscall.c`、`kernel/syscall.h` | Agent 系统调用号与分发表 |
| `kernel/proc.c`、`kernel/trap.c`、`kernel/vm.c` | 生命周期、scheduler、tick 和 Context 页权限接入 |

### 用户态与场景

| 文件 | 作用 |
| --- | --- |
| `user/user.h`、`user/usys.pl` | 用户态系统调用声明和汇编桩生成 |
| `user/planner_agent.c` | CodeLab Primary Agent 和工作流总控 |
| `user/retriever_agent.c` | AgentFS 查询和代码读取 |
| `user/patch_agent.c` | Lease 获取、补丁写入和结果上报 |
| `user/test_agent.c` | 调用动态规则测试工具 |
| `user/reviewer_agent.c` | FILEMOD 唤醒、diff 与测试结果审查 |
| `user/rule_test_tool_agent.c` | Tool-Service 与 `run_rule_test_dyn` |
| `user/llm_bridge.c` | xv6 内的 LLM 串口桥接 |
| `tools/llm_qemu_driver.py`、`tools/llm_proxy.py` | QEMU 自动驱动和宿主机模型代理 |

## 运行指南

### 运行 AgentOS

在宿主机进入仓库目录后执行：

```bash
make clean
make -j2
make qemu
```

`make` 会构建内核、用户程序和 `fs.img`。看到 xv6 shell 的 `$` 提示符后即可运行 AgentOS 程序；退出 QEMU 使用 `Ctrl-a x`。

### 运行功能测试

在 xv6 shell 中执行：

| 命令 | 验证内容 | 通过标志 |
| --- | --- | --- |
| `agenttest` | Agent 创建、Context、Tool、AgentFS 和 capability | `agenttest: all tests passed` |
| `agentlooptest` | 心跳、MESSAGE/FILEMOD、阻塞唤醒和多 Agent 调度 | `agentlooptest: all tests passed` |
| `agentfsbench` | 文件属性、索引查询与全量遍历 | `agentfsbench: all tests passed` |
| `agentinnov` | Schema、batch、动态工具、权限、共享缓存和超时接口 | `agentinnovationtest: all tests passed` |
| `agentlease` | Lease 冲突、版本提交、终止与过期回收 | `agentleasetest: all tests passed` |
| `agentorphan` | Primary/父进程退出、孤儿 Agent 和级联终止 | `agentorphan: all tests passed` |
| `codelabfail` | 动态工具服务退出后的失败传播和清理 | `[CodeLab-Fault] summary status=PASS` |

由于 xv6 `DIRSIZ` 限制，部分镜像内程序使用 `agentinnov`、`agentlease` 等短命令名。

### 运行性能量化测试

这些程序输出统一的 `[TEST]`、`[METRIC]` 和 `[SUMMARY]` 字段，便于宿主机脚本提取数据和绘图：

| 命令 | 主要对比或指标 |
| --- | --- |
| `agentperftest` | AgentFS、事件等待/唤醒和调度性能回归 |
| `agentfsmetric` | scan/index/cache 的 ticks、scanned、matches 和 cache hit |
| `contextmetric` | 首次查询、Context/缓存复用、失效和节省 ticks |
| `ctxvermetric` | 文件修改前后的 Context 版本状态与验证开销 |
| `waitmetric` | `agent_wait` 与轮询的唤醒延迟、循环数和 CPU ticks |
| `schedmetric` | dispatch、p50/p95/max wait 和公平性 |
| `mailbench` | FIFO accepted/received/busy、顺序错误、重复和丢弃统计 |

量化测试通过时应输出 `all tests passed` 或 `[SUMMARY] ... fail=0`；详细结果位于各条 `[METRIC]` 记录中。

### 运行 CodeLab

CodeLab 使用 `repo/` 中的示例代码仓库，演示多个 Agent 协作定位并修复 `todo.c` 的 delete bug。

#### 规则 Demo

先通过 `make qemu` 进入 xv6 shell，然后执行：

```text
planner_agent
```

这一模式不依赖外部模型，由内置规则规划并完整执行 Retriever、Patch、Test、Reviewer 和 Tool-Service 协作链。

#### LLM Demo

在宿主机执行：

```bash
python3 tools/llm_qemu_driver.py --mode demo
```

驱动会启动 QEMU，默认在 xv6 中运行 `planner_agent llm-api`，再由宿主机本地模拟代理通过串口返回响应，因此不需要 API key。也可以在已经启动的 xv6 shell 中直接运行 `planner_agent llm-demo`，使用程序内置的演示响应。

#### 接入真实大模型

配置兼容 Chat Completions 的模型服务后，在宿主机执行：

```bash
export AGENTOS_LLM_API_KEY="你的 API key"
export AGENTOS_LLM_MODEL="模型名称"
export AGENTOS_LLM_API_URL="Chat Completions 兼容接口地址"
python3 tools/llm_qemu_driver.py --mode api
```

真实模型负责返回是否启动 CodeLab 的规划结果；内核工具执行、文件修改和测试仍在 AgentOS 中完成。API key 和网络请求仅存在宿主机代理中，不进入 xv6 内核或镜像。

串口协议格式：

```text
@@AGENTOS_LLM_REQ id=planner role=planner prompt=fix todo delete bug @@END
@@AGENTOS_LLM_RESP id=planner state=done text=... action=start_codelab @@END
```

模型返回 `action=start_codelab` 后，`planner_agent llm-api` 才会执行多 Agent 修复流程。三种 CodeLab 运行方式最终都应看到：

```text
[AGENTOS] task=fix_todo_delete status=PASS
[RESULT] suite=dual target=agentos ... status=PASS
[METRIC] suite=dual target=agentos ... polling_loops=0 ... status=PASS
[Agent-Loop] final loop_state=5
```

## 场景功能与 AgentOS 功能对应表

| 场景功能 | AgentOS 功能 | 对应代码 |
| --- | --- | --- |
| Planner 启动并规划修复流程 | Primary Agent、心跳唤醒、工作流管理 | `user/planner_agent.c`、`kernel/agent_loop.c`、`kernel/workflow.c` |
| 五类 Worker 分工 | 角色、capability、Agent 发现和差异化调度 | `user/*_agent.c`、`kernel/agent.c` |
| Planner 向 Worker 分发任务 | 有界 FIFO 邮箱、结构化消息和阻塞唤醒 | `kernel/agent.c`、`kernel/agent_loop.c` |
| Retriever 按语义查找 `todo.c` | AgentFS 属性、摘要索引和结构化结果 | `user/retriever_agent.c`、`kernel/agent_fs.c` |
| Reviewer 重复相同查询 | 跨 Agent Shared Query Cache | `user/reviewer_agent.c`、`kernel/agent_fs.c` |
| Patch 修改 `repo/todo.c` | Tool Router、capability 检查和文件版本变化 | `user/patch_agent.c`、`kernel/agent_tool.c` |
| Reviewer 被修改事件唤醒 | FILEMOD、缓存失效和事件驱动等待 | `user/reviewer_agent.c`、`kernel/agent_loop.c` |
| Retriever 检查历史读取 | Context 文件依赖由 VALID 变为 STALE，重读后恢复 VALID | `user/retriever_agent.c`、`kernel/agent_context.c` |
| Test 调用 `run_rule_test_dyn` | 动态注册、请求转发、回复和失败传播 | `user/test_agent.c`、`user/rule_test_tool_agent.c`、`kernel/agent_tool.c` |
| Planner 输出最终 Summary | Context Path、文件摘要和工作流量化指标 | `user/planner_agent.c`、`kernel/workflow.c` |
| `llm-demo/llm-api` | LLM Bridge、宿主机 Proxy 和串口协议 | `user/llm_bridge.c`、`tools/llm_qemu_driver.py` |



## xv6 基础说明

本项目基于 xv6-riscv。原始 xv6 是 Dennis Ritchie 和 Ken Thompson 的 Unix Version 6 的教学操作系统重实现，RISC-V 版本由 MIT 6.1810 课程维护。本仓库在 xv6 基础上增加 AgentOS 相关内核扩展和用户态演示程序。
