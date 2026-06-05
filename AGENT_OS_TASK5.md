# 任务五实现说明

本文档只聚焦赛题任务五 `Agent Loop` 内核运行机制，说明这次具体改了哪些文件、为什么这样设计，以及 `user/agentlooptest.c` 每一部分在测试什么。

## 1. 本次实现覆盖了什么

本次实现覆盖了任务五中的这几项：

1. 心跳机制
   - `sys_agent_heartbeat_set`
   - `sys_agent_heartbeat_stop`
   - 心跳到达时由内核主动唤醒等待中的 Agent
   - Agent 可动态修改或停止心跳

2. 事件驱动触发
   - `sys_agent_watch`
   - `sys_agent_wait`
   - `sys_agent_unwatch`
   - 当前实现的事件类型为 `消息事件`
   - `send_message` 工具在目标 Agent 关注消息事件时，会触发内核唤醒

3. Agent Loop 生命周期管理
   - `agent_wait(1, ...)` 表示本轮结束后继续下一轮
   - `agent_wait(0, 0)` 表示任务完成，退出 Loop
   - `loop_state` 在 PCB 中维护

4. 多 Agent 协调运行
   - 多个 Agent 可同时阻塞等待
   - 不同 Agent 可被各自的心跳或消息事件唤醒

## 2. 改了哪些文件

### 2.1 内核接口与 ABI

改动文件：

- `kernel/agent.h`
- `kernel/syscall.h`
- `kernel/syscall.c`
- `kernel/sysagent.c`
- `user/user.h`
- `user/usys.pl`

新增内容：

1. 新的 Loop 状态和事件常量
   - `AGENT_LOOP_DONE`
   - `AGENT_EVENT_HEARTBEAT`
   - `AGENT_EVENT_MESSAGE`
   - `AGENT_WATCH_MESSAGE`

2. 新的用户态事件结构
   - `struct agent_wait_event`
   - 用来让 `agent_wait()` 把唤醒原因、触发 tick、消息内容复制给用户态

3. 新的系统调用
   - `agent_heartbeat_set`
   - `agent_heartbeat_stop`
   - `agent_watch`
   - `agent_wait`
   - `agent_unwatch`

为什么这样改：

- 任务五要求的是“内核级运行支持”，所以不能只把 `heartbeat_interval` 当元信息存起来，必须要有一套真正可等待、可唤醒的 syscall。
- `agent_wait()` 不能只返回整数，否则用户态不容易区分是谁唤醒了自己，也不方便读取事件附带的数据，所以加了 `agent_wait_event`。

### 2.2 PCB 扩展

改动文件：

- `kernel/proc.h`
- `kernel/agent.c`

新增字段：

- `heartbeat_deadline`
- `wakeup_tick`
- `watch_mask`
- `pending_events`
- `last_wakeup_reason`

为什么这样改：

- `heartbeat_deadline`：内核需要知道下一次心跳什么时候触发。
- `watch_mask`：Agent 需要注册自己关心哪些事件。
- `pending_events`：事件可能先到、Agent 后取，需要先在内核里挂起。
- `wakeup_tick`：测试和调试时要知道唤醒发生在什么时候。
- `last_wakeup_reason`：方便内核记录最近一次唤醒原因。

### 2.3 心跳触发路径

改动文件：

- `kernel/trap.c`
- `kernel/agent.c`

核心改动：

1. 在 `clockintr()` 里，在 `ticks++` 之后调用 `agent_tick(now)`。
2. `agent_tick(now)` 会扫描进程表：
   - 找出启用了心跳的 Agent
   - 判断 `now >= heartbeat_deadline`
   - 设置 `pending_events |= AGENT_EVENT_HEARTBEAT`
   - 更新下一次 `heartbeat_deadline`
   - 如果该 Agent 正在 `agent_wait()` 中睡眠，就把它切回 `RUNNABLE`

为什么这样改：

- 任务五要求“心跳到达时唤醒 Agent 进程”，最自然的挂载点就是时钟中断。
- 这样做不需要用户态轮询，也符合“Agent 在无事可做时真正休眠”的目标。

### 2.4 事件驱动消息唤醒

改动文件：

- `kernel/agent.c`

核心改动：

1. 保留已有 `send_message` 工具。
2. 在 `tool_send_message()` 中：
   - 把消息写入目标进程的 `agent_message`
   - 如果目标 Agent 注册了 `AGENT_WATCH_MESSAGE`
     - 设置 `pending_events |= AGENT_EVENT_MESSAGE`
     - 记录 `wakeup_tick`
     - 如果目标正在 `agent_wait()` 睡眠，则直接唤醒

为什么这样改：

- 赛题明确提到“收到 IPC 消息时唤醒 Agent”。
- 复用已有 `send_message` 工具可以最小化对任务二代码的破坏，同时把事件驱动触发真正接进内核等待路径。

### 2.5 `agent_wait()` 与生命周期管理

改动文件：

- `kernel/agent.c`

核心语义：

1. `agent_wait(1, &event)`
   - 表示本轮 Loop 结束后继续下一轮
   - 当前没有待处理事件时，进程进入 `SLEEPING`
   - 心跳或消息到来后返回
   - 把事件信息拷贝到用户态

2. `agent_wait(0, 0)`
   - 表示任务完成
   - 内核把 `loop_state` 置为 `AGENT_LOOP_DONE`
   - 清除心跳、关注事件和待处理事件

为什么这样改：

- 赛题要求 Agent 在每轮迭代结束时声明“继续”或“完成”。
- 这次没有再额外加一个“set_loop_state” syscall，而是把这层语义直接塞进 `agent_wait()`，这样用户态写 Loop 代码更自然：

```c
for(;;){
  // 思考 / 行动
  if(done)
    agent_wait(0, 0);
  else
    agent_wait(1, &event);
}
```

## 3. `agentlooptest` 测了什么

测试程序文件：

- `user/agentlooptest.c`

镜像集成：

- `Makefile` 中新增 `_agentlooptest`

### 3.1 `heartbeat_test()`

测试内容：

1. 调用 `agent_heartbeat_set(5)`
2. 调用 `agent_wait(1, &event)`
3. 检查返回原因包含 `AGENT_EVENT_HEARTBEAT`
4. 检查 `event.tick >= start + 5`

对应验收点：

- 心跳周期可配置
- 心跳到达时正确唤醒 Agent
- Agent 在等待期间不是立刻返回，而是真正睡到心跳到来

### 3.2 `message_only_test()`

测试内容：

1. 注册 `AGENT_WATCH_MESSAGE`
2. 调用 `agent_heartbeat_stop()`
3. fork 一个子进程，子进程延迟若干 tick 后用 `send_message` 发消息给父进程
4. 父进程调用 `agent_wait(1, &event)`
5. 检查返回原因是 `AGENT_EVENT_MESSAGE`
6. 检查消息内容正确

对应验收点：

- 支持事件驱动触发
- `sys_agent_watch / sys_agent_wait / sys_agent_unwatch` 可用
- 在关闭心跳后，消息事件仍能单独唤醒 Agent

### 3.3 `worker_loop()`

测试内容：

1. 子进程创建 Worker Agent
2. 注册消息关注
3. 启用心跳
4. 第一轮 `agent_wait(1, &event)` 等待心跳
5. 停止心跳，避免第二轮被下一个心跳抢先唤醒
6. 第二轮 `agent_wait(1, &event)` 等待消息
7. 检查 `agent_info().loop_state == AGENT_LOOP_READY`
8. 调用 `agent_wait(0, 0)` 声明任务完成
9. 检查 `agent_info().loop_state == AGENT_LOOP_DONE`

对应验收点：

- Agent Loop 的“继续/完成”生命周期管理
- `loop_state` 可由内核维护并查询

### 3.4 `multi_agent_test()`

测试内容：

1. fork 两个 Worker Agent
2. 两个 Worker 分别独立等待
3. 父进程分别向两个 Worker 发送消息
4. 检查两个 Worker 都能正常退出

对应验收点：

- 多个 Agent 同时运行，系统保持稳定
- 不同 Agent 的等待和唤醒互不干扰

## 4. 这次为什么没有做的更重

这次实现是“完成任务五要求”的最小闭环版本，刻意保持在 xv6 可控复杂度内：

1. 事件类型目前只实现了 `消息事件`
   - 没有继续扩展到“文件修改事件”等更多事件源
   - 这样可以先把等待/唤醒框架打稳

2. 没有单独实现优先级调度
   - 赛题把优先级列为“可选实现”
   - 当前仍使用 xv6 原有调度器，只在事件和心跳到来时把目标 Agent 标记为可运行

3. 没有引入新的复杂 IPC 子系统
   - 直接复用已有 `send_message` 工具，把它接到事件唤醒路径上

## 5. 如何运行测试

构建：

```bash
make
make fs.img
```

启动：

```bash
make qemu
```

在 xv6 shell 中运行：

```text
agentlooptest
```

期望结果：

```text
agentlooptest: all tests passed
```

建议顺手再跑一次原有回归：

```text
agenttest
```

期望结果：

```text
agenttest: all tests passed
```

## 6. 本次修改清单

本次主要改动文件：

- `kernel/agent.h`
- `kernel/proc.h`
- `kernel/agent.c`
- `kernel/sysagent.c`
- `kernel/syscall.h`
- `kernel/syscall.c`
- `kernel/trap.c`
- `kernel/defs.h`
- `user/user.h`
- `user/usys.pl`
- `user/agentlooptest.c`
- `Makefile`

如果你接下来还要继续冲高分，最自然的下一步是：

1. 在现有 `watch/wait` 框架上再增加新的事件源
2. 把 `send_message` 从“单消息槽”升级成消息队列
3. 把调度策略从“被动唤醒”扩展到“Agent 优先级/配额管理”
