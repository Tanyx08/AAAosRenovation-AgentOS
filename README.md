# Task 6 CodeLab 场景说明

## 1. 场景概述

这个场景模拟“多 Agent 协作修复一个小型代码仓库”的完整流程。

系统启动后，磁盘镜像中预置 `/repo` 目录，里面是一组简单的 todo 项目代码：

- `repo/main.c`：应用入口，调用 `add_task`、`delete_task`、`list_tasks` 等接口
- `repo/todo.c`：todo 模块实现，故意保留删除计数 bug
- `repo/todo.h`：todo 模块接口声明
- `repo/test.c`：规则测试样例
- `repo/README`：项目需求说明

当前任务是：

```text
修复 todo 删除任务后计数错误，并验证测试通过。
```

其中 `repo/todo.c` 的 bug 是：`delete_task()` 移动数组元素后没有执行 `task_count--`。

这个场景不是单独测试某一个 syscall，而是把 AgentOS 的多个能力串成一条可展示的闭环：

```text
规划 -> 检索 -> 读取 -> 修改 -> 测试 -> 审核 -> 总结
```

## 2. 当前实现了什么

当前版本已经实现并跑通以下能力：

- 主 Agent 通过心跳进入 Agent Loop。
- 主 Agent 创建多个子 Agent，并通过消息分配任务。
- Retriever-Agent 使用 AgentFS 的 `query_file` 和 `read_file` 定位 bug。
- Patch-Agent 使用 `patch_file` 修改 `repo/todo.c`。
- Test-Agent 不再直接调用内置 `run_rule_test`，而是调用动态注册工具 `run_rule_test_dyn`。
- Reviewer-Agent 监听 `repo/todo.c` 的 FILEMOD 事件，结合测试结果和 `diff_file` 给出审核结论。
- Retriever-Agent 第一次查询填充 shared query cache，Reviewer-Agent 重复查询时命中缓存。
- 各 Agent 设置不同调度优先级和配额，并在终端输出中展示。
- Planner-Agent 最终打印分角色 summary，并把自己的 loop 状态收尾为 `AGENT_LOOP_DONE`。
- 预留 LLM bridge/proxy 架构，支持规则模型、LLM demo 和未来真实 API 三种入口。

## 3. 参与角色

### Planner-Agent

程序：`planner_agent`

职责：

- 创建自己为 `PRIMARY Agent`
- 等待 HEARTBEAT 唤醒
- 生成或接收计划
- 创建子 Agent 和动态工具服务
- 设置并展示调度策略
- 分发任务消息
- 收集各阶段结果
- 打印最终摘要并结束 Agent Loop

### Retriever-Agent

程序：`retriever_agent`

职责：

- 接收 Planner 下发的检索任务
- 调用 `query_file(type=code;module=todo;keyword=delete)`
- 第一次查询填充 shared query cache
- 调用 `read_file(path=repo/todo.c)`
- 定位 `delete_task` 缺少 `task_count--`
- 通知 Patch-Agent 进行补丁修改

### Patch-Agent

程序：`patch_agent`

职责：

- 接收 Retriever 发送的补丁任务
- 调用 `patch_file`
- 将 `// BUG: missing task_count--` 替换为 `task_count--;`
- 修改文件后触发 FILEMOD 事件
- 通知 Test-Agent 进入测试阶段

### Test-Agent

程序：`test_agent`

职责：

- 接收 Patch-Agent 的“已修复”消息
- 调用动态工具 `run_rule_test_dyn`
- 检查 `todo.c` 中是否出现 `task_count--`
- 检查 `delete_task` 是否仍存在
- 检查 `test.c` 是否包含删除逻辑测试
- 将测试结果发给 Reviewer-Agent 和 Planner-Agent

### Reviewer-Agent

程序：`reviewer_agent`

职责：

- 监听 `repo/todo.c` 文件修改事件
- 在审核阶段重复执行相同查询，展示 shared query cache 命中
- 接收 Test-Agent 的测试结果
- 调用 `diff_file`
- 判断补丁和测试是否满足要求
- 向 Planner-Agent 返回 `approve` 或 `reject`

### Tool-Service

程序：`rule_test_tool_agent`

职责：

- 运行时注册动态工具 `run_rule_test_dyn`
- 等待 Test-Agent 通过 `tool_call` 调用
- 在用户态读取 `repo/todo.c` 和 `repo/test.c`
- 返回规则测试结果
- 完成一次服务后退出，展示动态工具生命周期

## 4. 展示的 AgentOS 能力

任务一：Agent 进程组织

`planner_agent` 创建并协调多个 Agent 进程，形成主 Agent 和多个专职子 Agent 的协作结构。

任务二：Tool Call 和动态工具

场景中使用 `query_file`、`read_file`、`patch_file`、`diff_file`、`send_message` 等工具。规则测试能力通过 `rule_test_tool_agent` 在运行时注册为 `run_rule_test_dyn`，证明工具能力可以动态扩展。

任务三：Context Path

各 Agent 在关键步骤调用 `context_push`，Planner-Agent 最终输出自己的 Context Path 摘要，展示任务执行轨迹。

任务四：AgentFS 查询优化

Retriever-Agent 通过文件属性和摘要查询定位代码文件，而不是硬编码路径。Reviewer-Agent 重复相同查询时命中 shared query cache，展示多个 Agent 共享查询结果。

任务五：Agent Loop 内核运行机制

场景中同时展示 HEARTBEAT、MESSAGE、FILEMOD 三类触发。Agent 在无事件时阻塞等待，事件到来后被内核唤醒。最终 Planner-Agent 将 loop 状态收尾为 `5`，即 `AGENT_LOOP_DONE`。

任务六：综合创新场景

整个 CodeLab 场景模拟一次“代码修复任务”，体现 AgentOS 对多 Agent 协作、文件系统语义查询、工具调用、动态工具、调度策略和未来 LLM 接入的综合支撑。

## 5. 调度策略展示

当前实现中，`agent_sched_set(priority, quota)` 是“当前进程设置自己”的接口，因此每个 Agent 在启动后设置自己的调度参数，再向 Planner-Agent 回报。

当前配置：

```text
Planner-Agent   sched=4/3  heartbeat=12
Retriever-Agent sched=8/8  高优先级，优先完成语义检索和 bug 定位
Patch-Agent     sched=8/7  高优先级，拿到定位结果后尽快修改文件
Test-Agent      sched=5/4  中等优先级，依赖 patch 完成后运行
Reviewer-Agent  sched=7/6  中高优先级，响应 FILEMOD 和测试消息
Tool-Service    sched=4/3  动态工具服务，按需运行
```

答辩时可以直接看终端中的 `[Scheduler]` 行和 worker ready 消息，例如：

```text
[Scheduler] Retriever-Agent high priority sched=8/8
[Scheduler] Patch-Agent high priority sched=8/7
[Patch-Agent] ready for MESSAGE wakeup; stage=patch;status=ready;sched=8/7
```

## 6. Shared Query Cache 展示

Retriever-Agent 第一次执行：

```text
query_file(type=code;module=todo;keyword=delete)
```

这次查询会走 AgentFS 索引并填充 shared query cache：

```text
[Kernel-FS] query_file type=code,module=todo,keyword=delete -> cache_hit=0, used_index=1
```

Reviewer-Agent 审核时重复相同查询：

```text
[Kernel-FS] reviewer repeated query -> shared query cache hit
```

最终 summary 会显示：

```text
[Summary] shared cache: retriever first query cache_hit=0; reviewer repeated query cache_hit=1
```

这证明多个 Agent 对同一代码仓库做重复查询时，第二次可以复用共享缓存，而不是重新遍历文件。

## 7. 动态工具注册展示

当前规则测试不是直接调用内置 `run_rule_test`，而是由 `rule_test_tool_agent` 动态注册：

```text
tool_register("run_rule_test_dyn", AGENT_TOOL_FLAG_PUBLIC)
```

Test-Agent 调用：

```text
tool_call("run_rule_test_dyn", "target=todo_delete")
```

终端输出示例：

```text
[Tool-Service] register run_rule_test_dyn ok; stage=tool;status=ready;tool=run_rule_test_dyn;sched=4/3
[Test-Agent] tool_call run_rule_test_dyn -> {status=ok,tool=run_rule_test_dyn,target=todo_delete,checks=CDT,passed=3,total=3}
```

其中 `checks=CDT` 表示：

- `C`：`task_count--` 检查通过
- `D`：`delete_task` 检查通过
- `T`：测试用例检查通过

## 8. LLM Bridge 和未来真实 API

当前默认模式不依赖真实 LLM，保证离线演示稳定。

支持三种入口：

```text
planner_agent
planner_agent llm-demo
planner_agent llm-api
```

默认规则模型：

```text
planner_agent
```

完全离线，Planner 使用固定规则计划，后续走稳定的多 Agent 链路。

LLM demo 模式：

```text
planner_agent llm-demo
```

会打印 bridge 协议请求和确定性 demo 响应：

```text
@@AGENTOS_LLM_REQ id=planner role=planner prompt=fix todo delete bug @@END
@@AGENTOS_LLM_RESP id=planner state=done text=plan_find_patch_test_review @@END
```

这个模式用于展示“未来 Planner 的计划可以来自 LLM bridge”，但执行阶段仍沿用当前稳定规则链。

独立 bridge 程序：

```text
llm_bridge demo fix_todo_delete
llm_bridge api fix_todo_delete
```

宿主机 proxy：

```bash
python3 tools/llm_proxy.py --mode demo
python3 tools/llm_proxy.py --mode api --api-key <key> --model <model> --api-url <url>
```

如果要让宿主机 proxy 和 QEMU 自动联动，使用 driver：

```bash
python3 tools/llm_qemu_driver.py --mode demo
python3 tools/llm_qemu_driver.py --mode api --api-key <key> --model <model> --api-url <url>
```

第三方 API 推荐配置方式：

```bash
export AGENTOS_LLM_API_KEY="你的第三方 API key"
export AGENTOS_LLM_MODEL="你的模型名称"
export AGENTOS_LLM_API_URL="第三方 OpenAI-compatible chat completions URL"
python3 tools/llm_proxy.py --mode api
```

也可以全部通过命令行传入：

```bash
python3 tools/llm_proxy.py --mode api \
  --api-key <key> \
  --model <model> \
  --api-url <url>
```

当前 proxy 使用 Python 标准库发 HTTP 请求，按 OpenAI-compatible chat completions 格式发送 `model/messages/temperature`。API key、网络请求、模型选择都留在宿主机侧；xv6 侧只负责发送紧凑请求、接收紧凑响应和执行工具。

### LLM 在当前 CodeLab 流程中的作用

当前真实 LLM 已经进入任务六主控路径，但它的职责是“策略层/授权层”，不是直接在 xv6 中改写 `todo.c`。

具体流程是：

```text
Planner-Agent 等待 HEARTBEAT
-> Planner-Agent 通过 bridge 向宿主机 LLM proxy 发出任务请求
-> 宿主机 proxy 调用第三方真实模型
-> 模型判断是否应该启动 CodeLab 修复
-> proxy 返回 action=start_codelab 或 action=abort
-> Planner-Agent 根据 action 决定是否创建 Retriever/Patch/Test/Reviewer
```

真实运行中可以看到类似输出：

```text
[Host-Proxy] send response: @@AGENTOS_LLM_RESP ... text=ACTION_start_codelab_REASON_User_requested_fix action=start_codelab @@END
[LLM-Bridge] received host response: ... action=start_codelab
[Summary] model: llm-api approved start_codelab
```

这表示真实模型完成了三件事：

- 理解任务请求，例如 `fix todo delete bug`
- 判断这个请求属于可以执行的 CodeLab 修复任务
- 授权 Planner-Agent 启动后续多 Agent 修复链路

后续的具体执行仍由 AgentOS 管理：

```text
query_file
read_file
patch_file
run_rule_test_dyn
diff_file
```

也就是说，当前架构不是让大模型直接任意修改文件，而是：

```text
LLM 负责决策
AgentOS 负责受控执行
Context / Tool / Event / Scheduler 负责记录、约束和推进流程
```

这样的设计更适合操作系统场景：大模型作为策略层给出高层意图，真正的文件修改、测试和审核通过内核管理的 Agent、工具调用、事件唤醒和调度机制完成，因此流程可控、可观察、可复现。

## 9. 如何启动测试

在仓库根目录编译并启动 xv6：

```bash
make qemu
```

如果只想重新生成镜像：

```bash
make fs.img
```

进入 xv6 shell 后运行默认场景：

```text
planner_agent
```

运行 LLM demo 场景：

```text
planner_agent llm-demo
```

单独测试 bridge：

```text
llm_bridge demo fix_todo_delete
```

宿主机 proxy demo 测试：

```bash
printf '@@AGENTOS_LLM_REQ id=1 role=planner prompt=fix_todo @@END\n' | python3 tools/llm_proxy.py --mode demo
```

宿主机自动驱动 QEMU + xv6 bridge：

```bash
python3 tools/llm_qemu_driver.py --mode demo
```

真实 API 自动联动：

```bash
export AGENTOS_LLM_API_KEY="你的第三方 API key"
export AGENTOS_LLM_MODEL="你的模型名称"
export AGENTOS_LLM_API_URL="第三方 chat completions URL"
python3 tools/llm_qemu_driver.py --mode api
```

这个 driver 会自动执行：

```text
make qemu CPUS=1
planner_agent llm-api
捕获 @@AGENTOS_LLM_REQ
调用 tools/llm_proxy.py 的 API 逻辑
把 @@AGENTOS_LLM_RESP 回写给 xv6
Planner-Agent 根据 action=start_codelab/abort 决定是否启动 CodeLab 修复链路
```

注意：xv6 console 的单行输入缓冲较小，proxy 回写给 `llm_bridge` 的响应必须保持紧凑。当前协议使用类似下面的短格式：

```text
@@AGENTOS_LLM_RESP id=2 state=done text=plan_patch_test action=start_codelab @@END
```

## 10. 当前输出规范

现在的终端输出统一使用分角色标签，便于答辩展示。

主要标签：

```text
[Planner-Agent]     主控 Agent 的规划、分发和总结
[Retriever-Agent]   检索、读取和 bug 定位
[Patch-Agent]       补丁修改
[Test-Agent]        动态工具测试调用
[Reviewer-Agent]    文件修改事件监听和最终审核
[Tool-Service]      动态工具注册
[Kernel]            Agent 创建
[Kernel-AgentLoop]  HEARTBEAT / MESSAGE 唤醒
[Kernel-FS]         AgentFS 查询、缓存和 FILEMOD
[Scheduler]         调度优先级和配额
[Summary]           最终结果摘要
[Context]           Planner 的 Context Path 摘要
[Agent-Loop]        Loop 生命周期收尾
```

一次成功运行应看到类似输出：

```text
[Planner-Agent] task: fix todo delete bug
[Planner-Agent] waiting for first HEARTBEAT
[Kernel-AgentLoop] HEARTBEAT -> wakeup Planner-Agent
[Planner-Agent] plan: find files -> inspect bug -> patch -> test -> review
[Planner-Agent] read repo bug marker from todo.c
[Kernel] create Retriever-Agent pid=4
[Kernel] create Patch-Agent pid=5
[Kernel] create Test-Agent pid=6
[Kernel] create Reviewer-Agent pid=7
[Kernel] create Tool-Service pid=8
[Scheduler] Retriever-Agent high priority sched=8/8
[Scheduler] Patch-Agent high priority sched=8/7
[Tool-Service] register run_rule_test_dyn ok; stage=tool;status=ready;tool=run_rule_test_dyn;sched=4/3
[Patch-Agent] ready for MESSAGE wakeup; stage=patch;status=ready;sched=8/7
[Test-Agent] ready for patched-file message; stage=test;status=ready;sched=5/4
[Reviewer-Agent] watch FILEMOD repo/todo.c; stage=reviewer;status=watching;file=repo/todo.c;sched=7/6
[Planner-Agent] send_message Retriever-Agent: find delete_task related files
[Kernel-AgentLoop] MESSAGE -> wakeup Retriever-Agent
[Kernel-FS] query_file type=code,module=todo,keyword=delete -> cache_hit=0, used_index=1
[Retriever-Agent] indexed AgentFS query populated shared cache
[Kernel-FS] reviewer repeated query -> shared query cache hit
[Retriever-Agent] read_file repo/todo.c -> found missing task_count--
[Patch-Agent] patch_file repo/todo.c replace BUG with task_count--
[Kernel-FS] FILEMOD repo/todo.c -> wakeup Reviewer-Agent
[Test-Agent] tool_call run_rule_test_dyn -> {status=ok,tool=run_rule_test_dyn,target=todo_delete,checks=CDT,passed=3,total=3}
[Reviewer-Agent] diff_file + rule result -> approve
[Planner-Agent] final summary
[Summary] shared cache: retriever first query cache_hit=0; reviewer repeated query cache_hit=1
[Summary] scheduling: planner=4/3 retriever=8/8 patch=8/7 test=5/4 reviewer=7/6 tool=4/3
[Summary] dynamic tool: run_rule_test_dyn registered and used by Test-Agent
[Summary] model: rule model demo
[Agent-Loop] final loop_state=5
```

如果运行 `planner_agent llm-demo`，还会在规划阶段看到：

```text
[LLM-Bridge] demo model selected; deterministic bridge plan is used
@@AGENTOS_LLM_REQ id=planner role=planner prompt=fix todo delete bug @@END
@@AGENTOS_LLM_RESP id=planner state=done text=plan_find_patch_test_review @@END
```

## 11. 验收点

一条成功的 Task 6 CodeLab 演示至少应该满足：

- `/repo` 是预置小型代码仓库，包含 `main.c`、`todo.c`、`todo.h`、`test.c`、`README`
- Planner-Agent 被 HEARTBEAT 唤醒
- Retriever-Agent 通过 AgentFS 查询定位文件
- 第一次查询 `cache_hit=0`，Reviewer 重复查询 `cache_hit=1`
- Patch-Agent 修改 `repo/todo.c`
- FILEMOD 事件唤醒 Reviewer-Agent
- Test-Agent 调用动态工具 `run_rule_test_dyn`
- 测试结果包含 `passed=3,total=3`
- Reviewer-Agent 返回 `approve`
- Summary 中展示 shared cache、scheduling、dynamic tool、model
- 最终输出 `[Agent-Loop] final loop_state=5`

## 12. 手动检查修复结果

在 xv6 shell 中可以查看修复后的文件：

```text
cat repo/todo.c
```

应能看到：

```c
task_count--;
```

也可以查看 bridge 程序是否存在：

```text
ls
```

由于 xv6 目录项长度限制，部分程序名会被截断。例如 `rule_test_tool_agent` 在 xv6 shell 中显示为 `rule_test_too`。

## 13. 答辩讲解顺序

推荐讲解顺序：

1. 先说明 `/repo` 是预置小型代码仓库，不是临时造数据。
2. 运行 `planner_agent`，展示主 Agent 被 HEARTBEAT 唤醒。
3. 说明 Planner 创建 Retriever、Patch、Test、Reviewer 和 Tool-Service。
4. 指出 `[Scheduler]` 行展示不同角色的优先级和配额。
5. 指出 `[Kernel-FS] cache_hit=0/1` 展示 shared query cache。
6. 指出 `[Tool-Service]` 和 `run_rule_test_dyn` 展示动态工具注册。
7. 指出 `[Kernel-FS] FILEMOD` 展示文件修改事件唤醒 Reviewer。
8. 最后展示 `[Summary]` 和 `[Agent-Loop] final loop_state=5`。
9. 如果需要讲未来 LLM，运行 `planner_agent llm-demo`，展示 `@@AGENTOS_LLM_REQ/RESP` 协议。

这样可以把任务四、任务五、任务六和创新点连成一条完整、清晰、可复现的演示链路。

## 14. 场景功能与 AgentOS 功能对应表

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
