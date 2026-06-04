# Agent-OS 使用手册与 LLM 演示指南

本文档面向使用者，目标是让你看完后知道如何构建系统、运行测试、调用 Agent-OS 接口，以及如何把宿主机上的真实 LLM 接入 xv6/QEMU 做演示。

如果你想了解内核内部实现，请看 `AGENT_OS_IMPLEMENTATION.md`。

## 1. 快速开始

进入仓库：

```bash
cd ~/workspace/xv6-2023-mit-labs
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

进入 xv6 shell 后运行测试：

```text
agenttest
```

期望结果：

```text
agenttest: all tests passed
```

完整回归测试：

```text
usertests
```

期望结果：

```text
ALL TESTS PASSED
```

验证 mmap：

```text
mmaptest
```

期望结果：

```text
mmaptest: all tests succeeded
```

退出 QEMU：

```text
Ctrl-A 然后按 X
```

## 2. 当前提供的用户态接口

接口声明在 `user/user.h`：

```c
uint64 agent_create(int type, int heartbeat_interval, uint64 quota);
int agent_info(void *info);
int tool_call(void *req, void *resp);
int tool_list(void *buf, uint64 len);
int context_push(void *node);
int context_query(void *buf, uint64 len);
int context_rollback(uint64 keep_nodes);
int context_clear(void);
```

结构体和常量定义在 `kernel/agent.h`。用户程序可以这样包含：

```c
#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"
```

## 3. 创建 Agent 进程

普通进程不能直接调用 Agent 工具。必须先调用 `agent_create()`。

示例：

```c
struct agent_info info;
char *ctx;

ctx = (char*)agent_create(AGENT_TYPE_PRIMARY, 25, 1024);
if((uint64)ctx == -1){
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
  心跳周期，目前作为内核元信息保存

quota:
  Context Path 最大字节配额
```

返回值：

```text
Agent Context 区起始地址
```

## 4. 读取 Agent Context 区

`agent_create()` 返回的地址指向一段用户态可读写的 Agent Context 区。

读取 header：

```c
struct agent_context_header *hdr;

hdr = (struct agent_context_header*)ctx;
if(hdr->magic == AGENT_CONTEXT_HEADER_MAGIC){
  printf("Agent Context ready\n");
}
```

读取 Context Path：

```c
char *path = ctx + hdr->path_offset;
write(1, path, hdr->path_length);
```

也可以通过 syscall 读取：

```c
char buf[512];
int n;

n = context_query(buf, sizeof(buf) - 1);
if(n > 0){
  buf[n] = 0;
  printf("%s\n", buf);
}
```

什么时候直接读，什么时候 syscall 读：

```text
直接读 Context 区:
  更快，无 syscall 开销，适合 Agent 高频访问上下文缓存

context_query:
  更适合测试、调试和演示
```

## 5. Tool Call 基本用法

推荐在用户程序里封装一个 helper：

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
  printf("tool not found\n");
}
```

## 6. 查看可用工具

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

## 7. 文件属性和 query_file

AgentFS 让 Agent 可以“按需求描述文件”，而不是必须知道完整路径。

### 7.1 创建演示文件

```c
int fd;

fd = open("agentbmem", O_CREATE | O_RDWR);
write(fd, "social memory alpha: Agent-B met Agent-A", 40);
close(fd);

fd = open("agentplan", O_CREATE | O_RDWR);
write(fd, "plan memory beta: Agent-B builds a route", 40);
close(fd);
```

xv6 文件名长度有限，建议使用短文件名，例如：

```text
agentbmem
agentplan
agentcfg
```

### 7.2 设置文件属性

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

### 7.3 查询文件属性

```c
call_tool("get_file_attr", "path=agentbmem;key=owner", &resp);
printf("%s\n", resp.result);
```

可能输出：

```text
{status=ok,path=agentbmem,owner=Agent-B}
```

### 7.4 按属性和内容摘要查询文件

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
path             找到的文件名
type/owner/tags  文件属性
summary          文件前 128 字节摘要
count            命中文件数量
used_index       是否使用属性索引
index_scanned    通过索引扫描的候选数量
full_scanned     当前元数据表中文件数量
```

这个例子展示了 Agent 友好的访问方式：

```text
传统方式:
  open("agentbmem")
  需要事先知道路径

Agent-OS:
  query_file(type=memory;owner=Agent-B;tags=social)
  只描述需求，由内核查找
```

## 8. Context Path 操作

每次 `tool_call()` 会自动追加 Context Path 节点。

手动追加：

```c
struct agent_context_node node;

memset(&node, 0, sizeof(node));
node.timestamp_ms = uptime();
strcpy(node.request, "manual request");
strcpy(node.result, "manual result");
context_push(&node);
```

读取：

```c
char buf[512];
int n = context_query(buf, sizeof(buf) - 1);
if(n > 0){
  buf[n] = 0;
  printf("%s\n", buf);
}
```

回滚：

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

## 9. 现有测试程序 agenttest

`user/agenttest.c` 是当前最完整的使用示例。

运行：

```text
agenttest
```

它会测试：

```text
普通进程 tool_call 拒绝
agent_create / agent_info
Context 区 header
tool_list
get_system_status
query_process
missing_tool 错误处理
set_file_attr / get_file_attr
query_file 属性和内容查询
Context Path 自动记录
context_query
context_rollback
context_clear
配额淘汰
```

如果你想写自己的 Agent 用户态程序，建议直接参考 `user/agenttest.c` 中的 `call_tool()`、`make_file()`、`set_attr()` 写法。

## 10. 真实 LLM 演示方案

真实 LLM 不建议运行在 xv6 内部，而是运行在宿主机上。xv6 中运行一个轻量 Agent 程序，负责接收 LLM 生成的工具调用并执行 syscall。

推荐架构：

```text
宿主机 LLM
  |
  | TOOL query_file type=memory;owner=Agent-B;tags=social;keyword=social
  v
宿主机脚本 / QEMU 串口
  |
  v
xv6 用户态 agent_loop
  |
  | tool_call()
  v
Agent-OS 内核
  |
  | query_file
  v
xv6 用户态 agent_loop
  |
  | OBS {status=ok,...}
  v
宿主机 LLM
```

真实演示目标可以设为：

```text
找到 Agent-B 的 social memory 文件。
```

预置文件：

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

LLM 应该输出：

```text
TOOL query_file type=memory;owner=Agent-B;tags=social;keyword=social
```

xv6 Agent-OS 返回：

```text
OBS {status=ok,files=[{path=agentbmem,...}],count=1,used_index=1,index_scanned=1,full_scanned=3}
```

最终 LLM 回答：

```text
目标文件是 agentbmem。
```

## 11. 建议新增 agent_loop.c

当前仓库已经实现内核接口和 `agenttest`，但还没有单独的 `agent_loop.c`。如果要做真实 LLM 演示，建议新增一个用户态程序：

```text
user/agent_loop.c
```

逻辑：

```text
1. 调用 agent_create()
2. 创建演示文件
3. 调用 set_file_attr 初始化属性
4. 打印 READY
5. 循环读取标准输入
6. 如果输入形如 TOOL <tool> <params>
7. 调用 tool_call()
8. 打印 OBS <resp.result>
```

伪代码：

```c
while(1){
  gets(line, sizeof(line));
  if(starts_with(line, "TOOL ")){
    parse tool and params;
    call_tool(tool, params, &resp);
    printf("OBS %s\n", resp.result);
  }
}
```

这样宿主机脚本就可以把 LLM 输出的一行 `TOOL ...` 发给 QEMU，再读取 `OBS ...`。

## 12. 宿主机脚本示例

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

如果还没有 `agent_loop`，可以先用 `agenttest` 验证内核功能；真实 LLM 演示需要补上这个循环程序。

## 13. LLM Prompt 示例

给真实 LLM 的系统提示可以写成：

```text
你是一个运行在宿主机上的 AI Agent。
你不能直接访问 xv6 文件系统。
你只能输出一行工具调用，格式如下：

TOOL <tool_name> <key=value;key=value>

可用工具：
query_file(type,owner,tags,keyword)
get_system_status()
query_process(type)
read_context()

任务：
找到 Agent-B 的 social memory 文件。
```

期望 LLM 输出：

```text
TOOL query_file type=memory;owner=Agent-B;tags=social;keyword=social
```

宿主机程序把这行发给 xv6 的 `agent_loop`，拿到 `OBS ...` 后再交回给 LLM，让它总结最终答案。

## 14. OpenAI API 接入示例

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

模型名称可根据实际 API 和账号权限调整。核心思想不变：

```text
LLM 负责决策
宿主机脚本负责转发
xv6 Agent-OS 负责执行工具调用
```

## 15. 演示时怎么讲

建议按下面顺序演示：

```text
1. 运行 agenttest，证明 Agent-OS 基础功能可用
2. 展示 query_file 不需要文件路径
3. 展示返回结果包含 used_index/index_scanned/full_scanned
4. 展示 Context Path 记录了多轮 tool_call
5. 运行宿主机 LLM 脚本，让 LLM 输出 TOOL
6. QEMU 返回 OBS
7. LLM 根据 OBS 总结结果
```

讲解重点：

```text
Agent 是 OS 可识别的一类进程
Agent 使用结构化 syscall 与内核交互
Agent Context 区支持用户态高速读取上下文
内核维护配额和上下文元信息
AgentFS 支持属性与摘要查询
真实 LLM 作为策略层运行在宿主机
```

## 16. 常见问题

### Q: 为什么不用 JSON？

xv6 内核环境很小，实现 JSON parser 成本高，也更容易引入边界 bug。当前 `key=value;key=value` 足够表达实验所需工具参数。

### Q: 文件属性会持久化吗？

当前不会。属性保存在内核内存表中，重启后消失。后续可以通过 `.agentmeta` 文件持久化。

### Q: query_file 是语义搜索吗？

不是。当前是属性匹配 + 前 128 字节摘要子串匹配。真实语义理解由宿主机 LLM 负责。

### Q: 真实 LLM 一定要 OpenAI 吗？

不需要。任何能输出 `TOOL ...` 文本的模型都可以接入。宿主机桥接脚本只需要把模型输出发给 QEMU，并把 `OBS ...` 交回模型。

### Q: 当前能直接做真实 LLM 演示吗？

内核接口已经具备。还需要补一个用户态 `agent_loop.c`，负责从标准输入读取 `TOOL ...` 并调用 `tool_call()`。
