# Agent-OS 性能测试方案

本文档对应用户态测试程序 `agentperftest`，用于把 Agent-OS 的性能观察项收敛成一组可在 QEMU 中直接运行、可记录输出的测试。测试重点不是追求真实生产环境基准分，而是在教学 OS 中证明任务四和任务五的关键机制确实生效。

## 运行方式

构建并进入 xv6：

```bash
make clean
make qemu
```

在 xv6 shell 中运行：

```text
agentperftest
```

期望最后输出：

```text
agentperftest: all tests passed
```

## 测试一：`query_file` 查询加速效果

测试目标：

```text
验证 AgentFS 属性索引查询比 mode=scan 全表扫描检查更少候选文件。
```

测试步骤：

```text
1. 创建 48 个小文件 pf00 到 pf47。
2. 给其中一部分文件设置 type/owner/tags 属性。
3. 构造同一组查询条件：
   type=perf;owner=Agent-P;tags=hot;keyword=needle
4. 分别运行默认索引查询和 mode=scan 全表扫描查询。
5. 解析 query_file 返回的 index_scanned/full_scanned/ticks_cost 等字段。
6. 批量运行 120 次索引查询和 120 次全表扫描查询，记录 batch_ticks。
```

记录数据：

```text
agentperftest: query_file index_scanned=<N> full_scanned=<M> batch_ticks=<T1>
agentperftest: query_file scan_scanned=<N> full_scanned=<M> batch_ticks=<T2>
```

预期现象：

```text
index_scanned < full_scanned
scan_scanned == full_scanned
索引查询能找到目标文件 pf17
```

说明：

```text
ticks 在小规模 xv6/QEMU 测试里可能为 0 或波动，因此 pass/fail 主要依据扫描候选数量，而不是绝对耗时。
```

## 测试二：心跳、消息、文件修改唤醒延迟

测试目标：

```text
验证 agent_wait 能被 HEARTBEAT、MESSAGE 和 FILEMOD 三类事件唤醒，并输出触发到返回的 tick 差值。
```

测试步骤：

```text
1. HEARTBEAT：设置 heartbeat_interval=6，进入 agent_wait，记录等待 tick。
2. MESSAGE：子 Agent sleep 后发送带 trigger tick 的消息，父 Agent 等待 MESSAGE。
3. FILEMOD：父 Agent watch 文件 pflog，子进程写文件前通过 pipe 记录 trigger tick，然后修改文件。
```

记录数据：

```text
agentperftest: latency heartbeat_wait_ticks=<T>
agentperftest: latency message_ticks=<T>
agentperftest: latency filemod_ticks=<T> file=pflog
```

预期现象：

```text
heartbeat_wait_ticks >= heartbeat_interval
message_ticks >= 0
filemod_ticks >= 0
```

说明：

```text
MESSAGE 和 FILEMOD 的 tick 差值通常很小；在 SMP QEMU 上可能出现轻微波动，但事件必须能稳定唤醒等待中的 Agent。
```

## 测试三：空闲时是否真正休眠

测试目标：

```text
验证无业务事件时 Agent Loop 可以通过 agent_wait 阻塞等待，而不是在用户态 busy loop 中空转。
```

测试步骤：

```text
1. 设置 heartbeat_interval=20。
2. 调用一次 agent_wait，直到心跳唤醒，记录 wait_ticks。
3. 在相同 tick 窗口内运行一个用户态轮询循环，统计 polling_loops。
```

记录数据：

```text
agentperftest: idle_wait returns=1 wait_ticks=<T> polling_loops=<N>
```

预期现象：

```text
wait_ticks >= 20
polling_loops > 1
```

解释口径：

```text
agent_wait 路径在空闲时只返回一次，表示 Agent 被内核挂起等待事件；轮询路径在同样时间内会执行大量循环，表示 busy loop 会浪费 CPU。
```

## 测试四：多 Agent 调度效果

测试目标：

```text
验证多个 Agent 同时运行时，priority/quota 设置会影响工作轮数。
```

测试步骤：

```text
1. fork 两个 Worker Agent。
2. 高优先级 Worker 设置 agent_sched_set(8, 8)。
3. 低优先级 Worker 设置 agent_sched_set(2, 1)。
4. 两者在相同 tick 窗口内持续计数。
5. 通过 pipe 把计数结果返回父进程。
```

记录数据：

```text
agentperftest: scheduler high_count=<H> low_count=<L> high_to_low_percent=<R>
```

预期现象：

```text
high_count > low_count
```

说明：

```text
该测试用于证明调度参数不是空接口。具体比例会受 QEMU、CPU 数和中断时机影响，因此只要求高优先级/高配额 Agent 的工作轮数更多。
```

## 本次测试结果与分析

以下结果来自一次在当前 `task6` 分支上运行 `agentperftest` 的实际输出。由于 xv6/QEMU 的 tick 粒度较粗，具体数值会随机器负载、QEMU 调度和 CPU 数略有波动；分析时应重点看相对关系和是否满足预期现象。

### 1. `query_file` 查询加速

实际输出：

```text
agentperftest: query_file index_scanned=13 full_scanned=83 batch_ticks=0
agentperftest: query_file scan_scanned=83 full_scanned=83 batch_ticks=0
```

分析：

```text
索引查询只检查 13 个候选文件，而全表扫描需要检查 83 个文件。
scan 模式下 scan_scanned == full_scanned，说明它确实绕过索引并遍历全集。
默认 query_file 使用属性索引后，候选集合约为全表扫描的 15.7%。
```

结论：

```text
AgentFS 属性索引有效减少了 query_file 的文件检查数量，能支撑任务四“语义化文件查询优化”的展示。
batch_ticks 为 0 是因为测试规模较小且 xv6 tick 粒度有限，因此这里不把绝对耗时作为主要判断依据。
```

### 2. 事件唤醒延迟

实际输出：

```text
agentperftest: latency heartbeat_wait_ticks=7
agentperftest: latency message_ticks=0
agentperftest: latency filemod_ticks=0 file=pflog
```

分析：

```text
heartbeat_interval 设置为 6，实际 heartbeat_wait_ticks 为 7，说明 Agent 没有忙等，而是在下一次满足心跳条件后被唤醒。
MESSAGE 和 FILEMOD 的触发 tick 到 wait 返回 tick 差值为 0，说明在当前 tick 粒度内事件能立即唤醒等待 Agent。
filemod 输出 file=pflog，说明 FILEMOD 事件不仅唤醒了 Agent，还携带了被修改文件路径。
```

结论：

```text
Agent Loop 能稳定响应心跳、消息和文件修改三类事件，满足任务五对事件驱动唤醒的要求。
```

### 3. 空闲休眠 vs 轮询空转

实际输出：

```text
agentperftest: idle_wait returns=1 wait_ticks=21 polling_loops=83418
```

分析：

```text
agent_wait 在约 21 个 tick 内只返回 1 次，说明 Agent 在无事件时由内核阻塞等待。
同样时间窗口内，用户态轮询循环执行了 83418 次，说明 busy loop 会持续消耗 CPU。
```

结论：

```text
Agent Loop 的等待机制可以避免空闲时反复轮询，适合解释“闲时睡眠、不浪费 CPU”的设计目标。
```

### 4. 多 Agent 调度效果

实际输出：

```text
agentperftest: scheduler high_count=285458 low_count=29993 high_to_low_percent=951
```

分析：

```text
高优先级/高配额 Worker 的工作计数为 285458。
低优先级/低配额 Worker 的工作计数为 29993。
high_to_low_percent=951 表示高优先级 Worker 的工作量约为低优先级 Worker 的 9.51 倍。
```

结论：

```text
priority/quota 设置会明显影响 Agent 获得 CPU 的机会，说明调度参数不是空实现。
该结果可以支撑任务五和创新点中“事件感知调度 / Agent 调度策略可观测”的展示。
```

### 总体结论

本次性能测试覆盖了查询、唤醒、空闲等待和调度四类指标：

```text
任务四：query_file 索引查询减少候选文件扫描数量。
任务五：agent_wait 能被 HEARTBEAT / MESSAGE / FILEMOD 稳定唤醒。
空闲效率：agent_wait 避免用户态 busy loop 空转。
调度效果：priority/quota 对多 Agent 工作量有可观测影响。
```

因此，`agentperftest` 不只是功能测试，也能作为答辩时的性能数据入口。展示时建议强调“相对关系稳定”，避免把 QEMU tick 下的绝对耗时解释成精密基准。

## 与已有测试的关系

`agentperftest` 是性能观察入口，和已有测试互补：

```text
agenttest        功能正确性：Agent 创建、工具调用、Context Path、AgentFS 基础能力
agentlooptest    功能正确性：心跳、消息、FILEMOD、多 Agent、调度策略
agentfsbench     AgentFS 查询索引专项 benchmark
agentperftest    统一性能测试：查询加速、唤醒延迟、空闲休眠、调度效果
```

答辩展示时可以先跑：

```text
agenttest
agentlooptest
agentfsbench
agentperftest
```

如果时间有限，优先展示 `agentperftest` 的四类指标输出。
