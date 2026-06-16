# Agent-OS 使用手册与 LLM 演示指南

本文档面向使用者和演示者，说明如何构建系统、运行测试、调用 Agent-OS 接口，以及如何把真实 LLM 作为策略层接入 xv6/QEMU。本文只讲“怎么用、怎么演示、怎么讲”，不重复展开内核实现细节。

内核实现细节请看 `AGENT_OS_IMPLEMENTATION.md`。

当前内核侧已经按职责拆分为：

```text
kernel/agent.c
  Agent 核心元信息与创建逻辑

kernel/agent_context.c
  Agent Context 区和 Context Path 管理

kernel/agent_fs.c
  AgentFS、文件属性、索引和共享查询缓存

kernel/agent_loop.c
  心跳、事件、agent_wait 和调度评分

kernel/agent_tool.c
  内置工具分发和动态工具注册/调用机制
```

## 1. 快速开始

进入仓库：

```bash
cd ~/gitStore/project3136859-388760
```

构建：

```bash
make clean
make
make fs.img
```

启动 xv6：

```bash
make qemu
```

进入 xv6 shell 后运行：

```text
agenttest
agentlooptest
agentfsbench
agentinnovationtest
```

期望结果：

```text
agenttest: all tests passed
agentlooptest: all tests passed
agentfsbench: all tests passed
agentinnovationtest: all tests passed
```

建议再跑原有回归：

```text
usertests
mmaptest
```

退出 QEMU：

```text
Ctrl-A 然后按 X
```

## 2. 用户态接口

系统调用声明位于 `user/user.h`：

```c
uint64 agent_create(int type, int heartbeat_interval, uint64 quota);
int agent_info(void *info);

int tool_call(void *req, void *resp);
int tool_list(void *buf, uint64 len);

int context_push(void *node);
int context_query(void *buf, uint64 len);
int context_rollback(uint64 keep_nodes);
int context_clear(void);

int agent_heartbeat_set(int interval);
int agent_heartbeat_stop(void);
int agent_watch(int mask);
int agent_watch_file(const char *path);
int agent_wait(int continue_loop, void *event);
int agent_unwatch(int mask);
int agent_priority_set(int priority);
int agent_sched_set(int priority, int quota);

int tool_register(const char *name, int flags);
int tool_recv(void *request);
int tool_reply(int request_id, const char *result, int status);
```

结构体和常量位于 `kernel/agent.h`。用户程序建议这样包含：

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"
```

## 3. 创建 Agent

普通进程不能调用 Agent 工具。需要先调用 `agent_create()` 把当前进程标记为 Agent。

```c
struct agent_info info;
char *ctx;

ctx = (char*)agent_create(AGENT_TYPE_PRIMARY, 25, 1024);
if((uint64)ctx == (uint64)-1){
  printf("agent_create failed\n");
  exit(1);
}

agent_info(&info);
printf("context_start=%p context_size=%d\n",
       info.context_start, info.context_size);
```

参数说明：

```text
type:
  AGENT_TYPE_PRIMARY  主 Agent
  AGENT_TYPE_WORKER   工作 Agent

heartbeat_interval:
  初始心跳周期，单位为 tick。为 0 表示创建时不启用心跳。

quota:
  Context Path 字节配额。为 0 或超过 Context 区容量时，使用最大可用容量。
```

返回值是用户态 Agent Context 区起始地址。该区域大小为 `AGENT_CONTEXT_REGION_SIZE`，当前为 8192 字节。

## 4. 读取 Agent Context 区

`agent_create()` 返回的地址指向用户态可读写的 Agent Context 区。区域开头是 header：

```c
struct agent_context_header *hdr;

hdr = (struct agent_context_header*)ctx;
if(hdr->magic == AGENT_CONTEXT_HEADER_MAGIC){
  printf("Agent Context ready\n");
}
```

直接读取 Context Path：

```c
char *path = ctx + hdr->path_offset;
write(1, path, hdr->path_length);
```

也可以通过 syscall 复制出来，适合测试和调试：

```c
char buf[512];
int n;

n = context_query(buf, sizeof(buf) - 1);
if(n > 0){
  buf[n] = 0;
  printf("%s\n", buf);
}
```

选择建议：

```text
直接读 Context 区:
  无 syscall 开销，适合 Agent 高频读取上下文缓存。

context_query:
  更适合测试、调试、演示和边界检查。
```

## 5. Tool Call 基本用法

建议在用户态封装一个 helper：

```c
static int
call_tool(const char *tool, const char *params,
          struct agent_tool_response *resp)
{
  struct agent_tool_request req;

  memset(&req, 0, sizeof(req));
  strcpy(req.tool, tool);
  strcpy(req.params, params);
  return tool_call(&req, resp);
}
```

调用系统状态工具：

```c
struct agent_tool_response resp;

call_tool("get_system_status", "", &resp);
printf("%s\n", resp.result);
```

可能输出：

```text
{status=ok,procs=4,agents=1,ticks=123}
```

查询 Agent 进程：

```c
call_tool("query_process", "type=agent", &resp);
printf("%s\n", resp.result);
```

可能输出：

```text
{status=ok,processes=[{pid=3,name=agenttest,type=1}],count=1}
```

错误处理：

```c
int rc = call_tool("missing_tool", "", &resp);
if(rc == AGENT_TOOL_ERR_TOOL_NOT_FOUND){
  printf("tool not found: %s\n", resp.result);
}
```

普通进程未调用 `agent_create()` 时调用 `tool_call()`，会返回：

```text
AGENT_TOOL_ERR_NOT_AGENT
```

## 6. 工具列表

调用：

```c
char tools[256];
int n;

n = tool_list(tools, sizeof(tools) - 1);
if(n > 0){
  tools[n] = 0;
  printf("%s\n", tools);
}
```

当前工具：

```text
get_system_status()
query_process(type)
send_message(target_pid,message)
read_context()
set_file_attr(path,key,value)
get_file_attr(path,key)
del_file_attr(path,key)
query_file(type,owner,tags,keyword)
```

参数格式统一为：

```text
key=value;key=value
```

例如：

```text
type=memory;owner=Agent-B;tags=social;keyword=social
```

## 7. 文件属性与 `query_file`

AgentFS 让 Agent 可以按属性和内容摘要查找文件，而不是必须事先知道路径。

### 7.1 创建演示文件

xv6 文件名长度有限，建议使用短文件名：

```c
int fd;

fd = open("agentbmem", O_CREATE | O_RDWR);
write(fd, "social memory alpha: Agent-B met Agent-A", 40);
close(fd);

fd = open("agentplan", O_CREATE | O_RDWR);
write(fd, "plan memory beta: Agent-B builds a route", 40);
close(fd);

fd = open("agentcfg", O_CREATE | O_RDWR);
write(fd, "runtime config gamma", 20);
close(fd);
```

### 7.2 设置属性

```c
call_tool("set_file_attr",
          "path=agentbmem;key=type;value=memory",
          &resp);
call_tool("set_file_attr",
          "path=agentbmem;key=owner;value=Agent-B",
          &resp);
call_tool("set_file_attr",
          "path=agentbmem;key=tags;value=social",
          &resp);
```

### 7.3 查询属性

```c
call_tool("get_file_attr", "path=agentbmem;key=owner", &resp);
printf("%s\n", resp.result);
```

可能输出：

```text
{status=ok,path=agentbmem,owner=Agent-B}
```

删除属性：

```c
call_tool("del_file_attr", "path=agentbmem;key=tags", &resp);
```

### 7.4 按属性和摘要查找文件

```c
call_tool("query_file",
          "type=memory;owner=Agent-B;tags=social;keyword=social",
          &resp);
printf("%s\n", resp.result);
```

可能输出：

```text
{status=ok,files=[{path=agentbmem,type=memory,owner=Agent-B,tags=social,summary=social memory alpha: Agent-B met Agent-A}],count=1,used_index=1,index_scanned=1,full_scanned=3}
```

字段解释：

```text
path             命中文件名
type/owner/tags  文件属性
summary          文件前 128 字节摘要
count            命中文件数量
used_index       是否使用属性索引
index_scanned    通过索引扫描的候选数量
full_scanned     当前元数据表中文件数量
```

对比：

```text
传统方式:
  open("agentbmem")
  Agent 必须事先知道路径。

Agent-OS:
  query_file(type=memory;owner=Agent-B;tags=social)
  Agent 描述需求，内核按属性和摘要查找。
```

## 8. Context Path 操作

每次 `tool_call()` 都会自动追加一个 Context Path 节点，记录请求和结果。

手动追加节点：

```c
struct agent_context_node node;

memset(&node, 0, sizeof(node));
node.timestamp_ms = uptime();
strcpy(node.request, "manual request");
strcpy(node.result, "manual result");
context_push(&node);
```

读取路径：

```c
char buf[512];
int n = context_query(buf, sizeof(buf) - 1);
if(n > 0){
  buf[n] = 0;
  printf("%s\n", buf);
}
```

回滚到前两个节点：

```c
context_rollback(2);
```

清空：

```c
context_clear();
```

配额淘汰示例：

```c
agent_create(AGENT_TYPE_WORKER, 10, 256);
for(int i = 0; i < 12; i++)
  call_tool("get_system_status", "", &resp);

agent_info(&info);
printf("dropped_nodes=%d\n", info.dropped_nodes);
```

## 9. Agent Loop 接口

Agent Loop 接口用于让 Agent 在无事可做时休眠，并被心跳或事件唤醒。

### 9.1 心跳

```c
agent_heartbeat_set(5);
```

含义：每 5 个 tick 触发一次心跳事件。停止心跳：

```c
agent_heartbeat_stop();
```

### 9.2 事件关注

当前支持消息事件和文件修改事件：

```c
agent_watch(AGENT_WATCH_MESSAGE);
agent_watch(AGENT_WATCH_FILEMOD);
agent_unwatch(AGENT_WATCH_MESSAGE);
```

### 9.3 等待下一轮

```c
struct agent_wait_event event;
int reason;

memset(&event, 0, sizeof(event));
reason = agent_wait(1, &event);

if(reason & AGENT_EVENT_HEARTBEAT)
  printf("heartbeat at tick %d\n", event.tick);

if(reason & AGENT_EVENT_MESSAGE)
  printf("message: %s\n", event.message);

if(reason & AGENT_EVENT_FILEMOD)
  printf("agentfs file metadata changed\n");
```

`agent_wait(1, &event)` 表示“本轮结束，继续下一轮”。如果当前没有 pending event，进程会进入睡眠状态，不忙等占 CPU。

声明任务完成：

```c
agent_wait(0, 0);
```

此时内核会将 `loop_state` 设置为 `AGENT_LOOP_DONE`，并清理心跳和事件关注。

## 10. Agent 消息事件

使用 `send_message` 工具向另一个 Agent 发送消息：

```c
call_tool("send_message",
          "target_pid=5;message=hello-worker",
          &resp);
```

目标 Agent 如果已经：

```c
agent_watch(AGENT_WATCH_MESSAGE);
agent_wait(1, &event);
```

就会被该消息唤醒，并通过 `event.message` 收到消息内容。

`user/agentlooptest.c` 展示了完整用法：

```text
父 Agent:
  fork worker
  sleep
  send_message(target_pid=worker,message=fanout-1)

Worker Agent:
  agent_create(...)
  agent_watch(AGENT_WATCH_MESSAGE)
  agent_wait(1, &event)
  检查 event.message
  agent_wait(0, 0)
```

### 10.1 文件修改事件和事件感知调度

`set_file_attr` 和 `del_file_attr` 会触发 `AGENT_EVENT_FILEMOD`。如果多个 Agent 同时变为 `RUNNABLE`，内核调度分数为：

```text
score = agent_priority * 10 + event_weight(pending_events) + aging_bonus
```

事件权重：

```text
MESSAGE > FILEMOD > HEARTBEAT
30        20        10
```

因此同优先级下，紧急消息会先于文件修改和心跳被处理。`agent_priority_set(5)` 可设置基础优先级，默认值为 5。

### 10.2 动态工具注册

动态工具类似 LLM skill：能力由用户态服务按需注册，调用方仍然使用统一的 `tool_call()`。

服务 Agent：

```c
struct agent_dynamic_tool_request req;

agent_create(AGENT_TYPE_WORKER, 0, 256);
tool_register("summarize_log", AGENT_TOOL_FLAG_PUBLIC);
tool_recv(&req);
tool_reply(req.request_id,
           "{status=ok,summary=agentlog compressed}",
           AGENT_TOOL_OK);
```

调用方 Agent：

```c
call_tool("summarize_log", "file=agentlog", &resp);
printf("%s\n", resp.result);
```

默认动态工具只允许同 Agent group 调用；注册时设置 `AGENT_TOOL_FLAG_PUBLIC` 后可跨组调用。

## 11. 现有测试程序

### 11.1 `agenttest`

运行：

```text
agenttest
```

覆盖内容：

```text
普通进程 tool_call 拒绝
agent_create / agent_info
Agent Context header
tool_list
get_system_status
query_process
missing_tool 错误处理
set_file_attr / get_file_attr
query_file 属性和摘要查询
Context Path 自动记录
context_query
context_rollback
context_clear
配额淘汰
```

### 11.2 `agentlooptest`

运行：

```text
agentlooptest
```

覆盖内容：

```text
heartbeat_test:
  agent_wait 被心跳唤醒

message_only_test:
  关闭心跳，只靠消息事件唤醒

worker_loop:
  心跳 -> 消息 -> DONE 生命周期

multi_agent_test:
  两个 Worker Agent 并发等待和唤醒
```

### 11.3 `agentfsbench`

运行：

```text
agentfsbench
```

覆盖内容：

```text
批量创建带属性文件
对比 query_file 索引查询和 mode=scan 全表扫描
验证 indexed query 的 index_scanned 小于 full_scanned
```

期望结果：

```text
agentfsbench: all tests passed
```

### 11.4 `agentinnovationtest`

运行：

```text
agentinnovationtest
```

覆盖内容：

```text
跨 Agent query_file 共享缓存:
  cache_hit=0 -> cache_hit=1，并验证命中后 fs_scanned=0

事件感知调度:
  MESSAGE、FILEMOD、HEARTBEAT 同时 pending 时按 M -> F -> H 运行

动态工具注册:
  summarize_log 工具服务注册、tool_recv 接单、tool_reply 返回结果
```

期望结果：

```text
agentinnovationtest: all tests passed
```

这三个测试基本就是最好的用户态示例。新增应用时，建议优先参考 `user/agenttest.c` 的 `call_tool()`、`make_file()`、`set_attr()`，`user/agentlooptest.c` 的 `send_message_to()`、`worker_loop()`，以及 `user/agentinnovationtest.c` 的动态工具服务写法。

## 12. LLM 演示架构

真实 LLM 不建议运行在 xv6 内部。推荐把 LLM 放在宿主机，xv6 内部只运行轻量 Agent 程序，通过串口收发结构化工具调用。

推荐架构：

```text
宿主机 LLM
  |
  | TOOL query_file type=memory;owner=Agent-B;tags=social;keyword=social
  v
宿主机桥接脚本 / QEMU 串口
  |
  v
xv6 用户态 Agent 程序
  |
  | tool_call()
  v
Agent-OS 内核
  |
  | query_file
  v
xv6 用户态 Agent 程序
  |
  | OBS {status=ok,...}
  v
宿主机 LLM
```

LLM 负责策略决策，Agent-OS 负责执行工具、维护上下文和提供结构化结果。

## 13. 演示目标设计

一个简洁的演示任务：

```text
找到 Agent-B 的 social memory 文件。
```

预置文件和属性：

```text
agentbmem:
  内容 social memory alpha: Agent-B met Agent-A
  type=memory
  owner=Agent-B
  tags=social

agentplan:
  内容 plan memory beta: Agent-B builds a route
  type=memory
  owner=Agent-B
  tags=plan

agentcfg:
  内容 runtime config gamma
  type=config
  owner=Agent-A
  tags=runtime
```

LLM 应输出：

```text
TOOL query_file type=memory;owner=Agent-B;tags=social;keyword=social
```

xv6 Agent-OS 返回：

```text
OBS {status=ok,files=[{path=agentbmem,...}],count=1,used_index=1,index_scanned=1,full_scanned=3}
```

最终 LLM 总结：

```text
目标文件是 agentbmem。
```

## 14. 建议的用户态 `agent_loop`

当前仓库的自动测试已经覆盖内核接口。如果要做真实 LLM 串口演示，建议新增一个用户态程序，例如：

```text
user/agent_loop.c
```

核心流程：

```text
1. agent_create(AGENT_TYPE_PRIMARY, ...)
2. 创建演示文件
3. set_file_attr 初始化属性
4. 打印 READY
5. 循环 gets() 读取标准输入
6. 解析 TOOL <tool> <params>
7. 调用 tool_call()
8. 打印 OBS <resp.result>
```

伪代码：

```c
for(;;){
  gets(line, sizeof(line));
  if(starts_with(line, "TOOL ")){
    parse_tool_line(line, tool, params);
    call_tool(tool, params, &resp);
    printf("OBS %s\n", resp.result);
  }
}
```

这层用户态程序是桥接层，不承载复杂智能；真正的推理仍由宿主机 LLM 完成。

## 15. 宿主机桥接脚本示例

可以用 Python `pexpect` 驱动 QEMU：

```python
import pexpect

qemu = pexpect.spawn("make qemu", encoding="utf-8", timeout=60)
qemu.expect("\\$ ")
qemu.sendline("agent_loop")

qemu.expect("READY")
qemu.sendline(
    "TOOL query_file type=memory;owner=Agent-B;tags=social;keyword=social"
)
qemu.expect("OBS .*")
print(qemu.after)

qemu.sendcontrol("a")
qemu.send("x")
```

如果暂时没有 `agent_loop`，可以先运行 `agenttest` 和 `agentlooptest` 展示内核能力。

## 16. LLM Prompt 示例

给 LLM 的系统提示可以写成：

```text
你是一个运行在宿主机上的 AI Agent。
你不能直接访问 xv6 文件系统。
你只能输出一行工具调用，格式如下：

TOOL <tool_name> <key=value;key=value>

可用工具：
get_system_status()
query_process(type)
read_context()
query_file(type,owner,tags,keyword)

任务：
找到 Agent-B 的 social memory 文件。
```

期望 LLM 输出：

```text
TOOL query_file type=memory;owner=Agent-B;tags=social;keyword=social
```

桥接脚本把这行发给 xv6 的 `agent_loop`，再把 `OBS ...` 作为观察结果交回 LLM，让它生成最终答案。

## 17. OpenAI API 接入示例

以下代码运行在宿主机，不运行在 xv6 内。

```python
from openai import OpenAI
import pexpect

client = OpenAI()

qemu = pexpect.spawn("make qemu", encoding="utf-8", timeout=60)
qemu.expect("\\$ ")
qemu.sendline("agent_loop")
qemu.expect("READY")

messages = [
    {
        "role": "system",
        "content": (
            "你是一个 Agent。你只能通过 TOOL 行调用 xv6 Agent-OS。"
            "格式：TOOL <tool> <params>。"
        ),
    },
    {
        "role": "user",
        "content": "找到 Agent-B 的 social memory 文件。",
    },
]

resp = client.chat.completions.create(
    model="gpt-4.1-mini",
    messages=messages,
)

tool_line = resp.choices[0].message.content.strip()
qemu.sendline(tool_line)
qemu.expect("OBS .*")
observation = qemu.after

messages.append({"role": "assistant", "content": tool_line})
messages.append({"role": "user", "content": observation})

final = client.chat.completions.create(
    model="gpt-4.1-mini",
    messages=messages,
)

print(final.choices[0].message.content)
```

模型名称按实际账号权限调整。核心不变：

```text
LLM 负责决策
桥接脚本负责串口转发
xv6 Agent-OS 负责工具执行和上下文维护
```

## 18. 演示讲解顺序

建议按下面顺序讲：

```text
1. 运行 agenttest，证明 Agent 进程、Tool Call、Context Path、AgentFS 可用
2. 展示 query_file 不需要完整路径，只需要属性和 keyword
3. 展示 used_index / index_scanned / full_scanned，说明查询优化
4. 运行 agentinnovationtest，展示共享缓存、事件感知调度和动态工具注册
5. 展示 Context Path 记录多轮工具调用
6. 运行 agentlooptest，证明心跳、消息事件和多 Agent Loop 可用
7. 展示宿主机 LLM 输出 TOOL
8. QEMU 返回 OBS
9. LLM 基于 OBS 总结最终答案
```

讲解重点：

```text
Agent 是 OS 可识别的一类进程
Agent 通过结构化 syscall 与内核交互
Agent Context 区支持用户态高速读取上下文
内核维护上下文元信息、配额、安全检查和唤醒机制
AgentFS 支持属性和摘要查询
共享查询缓存避免多个 Agent 重复扫描
Agent Loop 可由心跳、消息或文件修改事件驱动
动态工具注册让用户态服务像 skill 一样扩展工具能力
真实 LLM 作为策略层运行在宿主机
```

## 19. 常见问题

### Q: 为什么不用 JSON？

xv6 内核环境很小，实现 JSON parser 成本高，也更容易引入边界问题。当前 `key=value;key=value` 足够表达实验所需工具参数，并且容易在内核中解析。

### Q: 文件属性会持久化吗？

会。合并远端实现后，属性和摘要已经写入 inode/dinode；内核仍会维护运行时索引缓存来加速 `query_file`。

### Q: `query_file` 是语义搜索吗？

不是。当前是属性匹配加前 128 字节摘要子串匹配。真实语义理解由宿主机 LLM 负责。

### Q: 多 Agent 有专门优先级调度吗？

有。当前调度器会综合 `agent_priority`、远端的 `agent_sched_set(priority, quota)`、事件权重和 aging；同优先级下 MESSAGE 优先于 FILEMOD，FILEMOD 优先于 HEARTBEAT。普通进程仍有默认分和 aging，避免长期饥饿。

### Q: 真实 LLM 一定要 OpenAI 吗？

不需要。任何能输出 `TOOL ...` 文本的模型都可以接入。桥接脚本只需要把模型输出发给 QEMU，并把 `OBS ...` 交回模型。

### Q: 没有 `agent_loop` 时怎么演示？

先用 `agenttest`、`agentlooptest` 和 `agentinnovationtest` 展示内核功能；如果要做真实 LLM 闭环，再补一个很薄的 `agent_loop` 用户态桥接程序。
