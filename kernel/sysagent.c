// Agent-OS 系统调用桥接层。
//
// 这个文件包含 Agent-OS 子系统的系统调用入口封装。每个封装函数负责：
//  从用户态复制参数、调用 agent.c 中对应的 Agent 内核辅助函数，并在需要时
//  把结构化结果复制回用户态。它位于通用 syscall 分发层与 Agent 运行时核心
//  之间，承担参数转换与边界适配的职责。

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "agent.h"

uint64
sys_agent_create(void)
{
  int type;
  int heartbeat_interval;
  uint64 quota;

  argint(0, &type);
  argint(1, &heartbeat_interval);
  argaddr(2, &quota);
  return agent_mark_current(type, heartbeat_interval, quota);
}

uint64
sys_agent_info(void)
{
  uint64 uinfo;
  struct agent_info info;
  struct proc *p = myproc();

  argaddr(0, &uinfo);
  agent_get_info(p, &info);
  if(copyout(p->pagetable, uinfo, (char*)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}

uint64
sys_tool_call(void)
{
  uint64 ureq;
  uint64 uresp;
  struct agent_tool_request *req;
  struct agent_tool_response *resp;
  struct proc *p = myproc();
  int status;

  argaddr(0, &ureq);
  argaddr(1, &uresp);
  req = (struct agent_tool_request*)kalloc();
  resp = (struct agent_tool_response*)kalloc();
  if(req == 0 || resp == 0){
    if(req)
      kfree((void*)req);
    if(resp)
      kfree((void*)resp);
    return -1;
  }
  if(copyin(p->pagetable, (char*)req, ureq, sizeof(*req)) < 0){
    kfree((void*)req);
    kfree((void*)resp);
    return -1;
  }
  req->tool[sizeof(req->tool) - 1] = 0;
  req->params[sizeof(req->params) - 1] = 0;
  agent_tool_call(p, req, resp);
  status = resp->status;
  if(copyout(p->pagetable, uresp, (char*)resp, sizeof(*resp)) < 0)
    status = -1;
  kfree((void*)req);
  kfree((void*)resp);
  return status;
}

uint64
sys_tool_list(void)
{
  uint64 ubuf;
  uint64 len;

  argaddr(0, &ubuf);
  argaddr(1, &len);
  return agent_copy_tool_list(myproc(), ubuf, len);
}

uint64
sys_context_push(void)
{
  uint64 unode;
  struct agent_context_node *node;
  struct proc *p = myproc();
  int ret;

  argaddr(0, &unode);
  node = (struct agent_context_node*)kalloc();
  if(node == 0)
    return -1;
  if(copyin(p->pagetable, (char*)node, unode, sizeof(*node)) < 0){
    kfree((void*)node);
    return -1;
  }
  node->request[sizeof(node->request) - 1] = 0;
  node->result[sizeof(node->result) - 1] = 0;
  ret = agent_context_push_node(p, node);
  kfree((void*)node);
  return ret;
}

uint64
sys_context_query(void)
{
  uint64 ubuf;
  uint64 len;

  argaddr(0, &ubuf);
  argaddr(1, &len);
  return agent_context_query(myproc(), ubuf, len);
}

uint64
sys_context_rollback(void)
{
  int keep_nodes;

  argint(0, &keep_nodes);
  return agent_context_rollback(myproc(), keep_nodes);
}

uint64
sys_context_clear(void)
{
  agent_context_clear(myproc());
  return 0;
}

uint64
sys_agent_heartbeat_set(void)
{
  int interval;

  argint(0, &interval);
  return agent_proc_heartbeat_set(myproc(), interval);
}

uint64
sys_agent_heartbeat_stop(void)
{
  return agent_proc_heartbeat_stop(myproc());
}

uint64
sys_agent_watch(void)
{
  int mask;

  argint(0, &mask);
  return agent_proc_watch(myproc(), mask);
}

uint64
sys_agent_wait(void)
{
  int continue_loop;
  uint64 uevent;

  argint(0, &continue_loop);
  argaddr(1, &uevent);
  return agent_proc_wait(myproc(), continue_loop, uevent);
}

uint64
sys_agent_unwatch(void)
{
  int mask;

  argint(0, &mask);
  return agent_proc_unwatch(myproc(), mask);
}

uint64
sys_agent_priority_set(void)
{
  int priority;

  argint(0, &priority);
  return agent_proc_priority_set(myproc(), priority);
}

uint64
sys_tool_register(void)
{
  char name[AGENT_TOOL_NAME_MAX];
  int flags;

  if(argstr(0, name, sizeof(name)) < 0)
    return -1;
  argint(1, &flags);
  return agent_tool_register(myproc(), name, flags);
}

uint64
sys_tool_recv(void)
{
  uint64 ureq;
  struct agent_dynamic_tool_request req;
  struct proc *p = myproc();
  int ret;

  argaddr(0, &ureq);
  ret = agent_tool_recv(p, &req);
  if(ret < 0)
    return ret;
  if(copyout(p->pagetable, ureq, (char*)&req, sizeof(req)) < 0)
    return -1;
  return 0;
}

uint64
sys_tool_reply(void)
{
  int request_id;
  int status;
  char result[AGENT_TOOL_RESULT_MAX];

  argint(0, &request_id);
  if(argstr(1, result, sizeof(result)) < 0)
    return -1;
  argint(2, &status);
  return agent_tool_reply(myproc(), request_id, result, status);
}

uint64
sys_agent_watch_file(void)
{
  uint64 upath;

  argaddr(0, &upath);
  return agent_proc_watch_file(myproc(), upath);
}

uint64
sys_agent_sched_set(void)
{
  int priority;
  int quota;

  argint(0, &priority);
  argint(1, &quota);
  return agent_proc_sched_set(myproc(), priority, quota);
}
