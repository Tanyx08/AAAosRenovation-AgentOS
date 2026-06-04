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
  struct agent_tool_request req;
  struct agent_tool_response resp;
  struct proc *p = myproc();

  argaddr(0, &ureq);
  argaddr(1, &uresp);
  if(copyin(p->pagetable, (char*)&req, ureq, sizeof(req)) < 0)
    return -1;
  req.tool[sizeof(req.tool) - 1] = 0;
  req.params[sizeof(req.params) - 1] = 0;
  agent_tool_call(p, &req, &resp);
  if(copyout(p->pagetable, uresp, (char*)&resp, sizeof(resp)) < 0)
    return -1;
  return resp.status;
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
  struct agent_context_node node;
  struct proc *p = myproc();

  argaddr(0, &unode);
  if(copyin(p->pagetable, (char*)&node, unode, sizeof(node)) < 0)
    return -1;
  node.request[sizeof(node.request) - 1] = 0;
  node.result[sizeof(node.result) - 1] = 0;
  return agent_context_push_node(p, &node);
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
  uint64 keep_nodes;

  argaddr(0, &keep_nodes);
  return agent_context_rollback(myproc(), keep_nodes);
}

uint64
sys_context_clear(void)
{
  agent_context_clear(myproc());
  return 0;
}
