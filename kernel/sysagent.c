// Agent-OS 系统调用桥接层。
//
// 决赛新增 syscall: agent_wait_timeout, tool_call_batch, tool_schema,
// agent_cap_set, agent_lease_begin/commit/abort, agent_query_agent, agent_role_set

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
  int type, heartbeat_interval; uint64 quota;
  argint(0, &type); argint(1, &heartbeat_interval); argaddr(2, &quota);
  return agent_mark_current(type, heartbeat_interval, quota);
}

uint64
sys_agent_info(void)
{
  uint64 uinfo; struct agent_info info; struct proc *p = myproc();
  argaddr(0, &uinfo);
  agent_get_info(p, &info);
  if(copyout(p->pagetable, uinfo, (char*)&info, sizeof(info)) < 0) return -1;
  return 0;
}

uint64
sys_tool_call(void)
{
  uint64 ureq, uresp; struct agent_tool_request *req; struct agent_tool_response *resp;
  struct proc *p = myproc(); int status;
  argaddr(0, &ureq); argaddr(1, &uresp);
  req = (struct agent_tool_request*)kalloc(); resp = (struct agent_tool_response*)kalloc();
  if(req == 0 || resp == 0){ if(req) kfree((void*)req); if(resp) kfree((void*)resp); return -1; }
  if(copyin(p->pagetable, (char*)req, ureq, sizeof(*req)) < 0){ kfree((void*)req); kfree((void*)resp); return -1; }
  req->tool[sizeof(req->tool) - 1] = 0; req->params[sizeof(req->params) - 1] = 0;
  agent_tool_call(p, req, resp); status = resp->status;
  if(copyout(p->pagetable, uresp, (char*)resp, sizeof(*resp)) < 0) status = -1;
  kfree((void*)req); kfree((void*)resp); return status;
}

uint64
sys_tool_list(void)
{ uint64 ubuf, len; argaddr(0, &ubuf); argaddr(1, &len); return agent_copy_tool_list(myproc(), ubuf, len); }

uint64
sys_context_push(void)
{
  uint64 unode; struct agent_context_node *node; struct proc *p = myproc(); int ret;
  argaddr(0, &unode);
  node = (struct agent_context_node*)kalloc();
  if(node == 0) return -1;
  if(copyin(p->pagetable, (char*)node, unode, sizeof(*node)) < 0){ kfree((void*)node); return -1; }
  node->request[sizeof(node->request) - 1] = 0; node->result[sizeof(node->result) - 1] = 0;
  ret = agent_context_push_node(p, node); kfree((void*)node); return ret;
}

uint64
sys_context_query(void)
{ uint64 ubuf, len; argaddr(0, &ubuf); argaddr(1, &len); return agent_context_query(myproc(), ubuf, len); }

uint64
sys_context_rollback(void)
{ int keep_nodes; argint(0, &keep_nodes); return agent_context_rollback(myproc(), keep_nodes); }

uint64
sys_context_clear(void)
{ agent_context_clear(myproc()); return 0; }

uint64
sys_context_validate(void)
{
  uint64 sequence, result;

  argaddr(0, &sequence);
  argaddr(1, &result);
  return agent_context_validate(myproc(), sequence, result);
}

uint64
sys_agent_heartbeat_set(void)
{ int interval; argint(0, &interval); return agent_proc_heartbeat_set(myproc(), interval); }

uint64
sys_agent_heartbeat_stop(void)
{ return agent_proc_heartbeat_stop(myproc()); }

uint64
sys_agent_watch(void)
{ int mask; argint(0, &mask); return agent_proc_watch(myproc(), mask); }

uint64
sys_agent_wait(void)
{
  int continue_loop; uint64 uevent;
  argint(0, &continue_loop); argaddr(1, &uevent);
  return agent_proc_wait(myproc(), continue_loop, uevent, 0);
}

// 修改点 #17: agent_wait 带 timeout
uint64
sys_agent_wait_timeout(void)
{
  int continue_loop, timeout_ticks; uint64 uevent;
  argint(0, &continue_loop); argint(1, &timeout_ticks); argaddr(2, &uevent);
  return agent_proc_wait(myproc(), continue_loop, uevent, timeout_ticks);
}

uint64
sys_agent_unwatch(void)
{ int mask; argint(0, &mask); return agent_proc_unwatch(myproc(), mask); }

uint64
sys_agent_priority_set(void)
{ int priority; argint(0, &priority); return agent_proc_priority_set(myproc(), priority); }

uint64
sys_tool_register(void)
{ char name[AGENT_TOOL_NAME_MAX]; int flags;
  if(argstr(0, name, sizeof(name)) < 0)
    return -1;
  argint(1, &flags);
  return agent_tool_register(myproc(), name, flags); }

uint64
sys_tool_recv(void)
{
  uint64 ureq; struct agent_dynamic_tool_request req; struct proc *p = myproc(); int ret;
  argaddr(0, &ureq); ret = agent_tool_recv(p, &req);
  if(ret < 0) return ret;
  if(copyout(p->pagetable, ureq, (char*)&req, sizeof(req)) < 0) return -1;
  return 0;
}

uint64
sys_tool_reply(void)
{
  int request_id, status; char *result; int ret;
  argint(0, &request_id); result = kalloc();
  if(result == 0) return -1;
  if(argstr(1, result, AGENT_TOOL_RESULT_MAX) < 0){ kfree(result); return -1; }
  argint(2, &status); ret = agent_tool_reply(myproc(), request_id, result, status);
  kfree(result); return ret;
}

uint64
sys_agent_watch_file(void)
{ uint64 upath; argaddr(0, &upath); return agent_proc_watch_file(myproc(), upath); }

uint64
sys_agent_sched_set(void)
{ int priority, quota; argint(0, &priority); argint(1, &quota);
  return agent_proc_sched_set(myproc(), priority, quota); }

// ---- 决赛新增 syscall ----

// 修改点 #22: tool_call_batch
uint64
sys_tool_call_batch(void)
{
  uint64 ubatch; struct agent_tool_batch_request breq; int ret;
  struct agent_tool_batch_response *bresp; struct proc *p = myproc();
  argaddr(0, &ubatch);
  bresp = (struct agent_tool_batch_response*)kalloc();
  if(bresp == 0) return -1;
  if(copyin(p->pagetable, (char*)&breq, ubatch, sizeof(breq)) < 0)
  { kfree((void*)bresp); return -1; }
  memset(bresp, 0, sizeof(*bresp));
  ret = agent_tool_call_batch(p, &breq, bresp);
  if(copyout(p->pagetable, ubatch, (char*)bresp, sizeof(*bresp)) < 0) ret = -1;
  kfree((void*)bresp); return ret;
}

// 修改点 #14: tool_schema 查询
uint64
sys_tool_schema(void)
{
  uint64 uname, uschema; char name[AGENT_TOOL_NAME_MAX];
  struct agent_tool_schema schema; struct proc *p = myproc();
  argaddr(0, &uname); argaddr(1, &uschema);
  if(copyinstr(p->pagetable, name, uname, sizeof(name)) < 0) return -1;
  if(agent_tool_schema_get(p, name, &schema) < 0) return -1;
  if(copyout(p->pagetable, uschema, (char*)&schema, sizeof(schema)) < 0) return -1;
  return 0;
}

// 修改点 #3: agent_cap_set
uint64
sys_agent_cap_set(void)
{
  uint64 caps; argaddr(0, &caps);
  return agent_proc_cap_set(myproc(), caps);
}

// 修改点 #2: lease_begin
uint64
sys_agent_lease_begin(void)
{
  uint64 upath; uint64 uresult; char path[AGENT_MESSAGE_MAX];
  char result[64]; uint64 lease_id, base_version; int ret;
  struct proc *p = myproc();
  argaddr(0, &upath); argaddr(1, &uresult);
  if(copyinstr(p->pagetable, path, upath, sizeof(path)) < 0) return -1;
  ret = agent_proc_lease_begin(p, path, &lease_id, &base_version);
  if(ret != 0) return ret;
  memset(result, 0, sizeof(result));
  // 手动构造结果字符串
  safestrcpy(result, "lease_id=", sizeof(result));
  int off = strlen(result);
  // 简单数字转换
  char tmp[24]; int n = 0; uint64 v = lease_id;
  if(v == 0) tmp[n++] = '0';
  else { while(v > 0 && n < (int)sizeof(tmp)-1){ tmp[n++] = '0' + (v % 10); v /= 10; } }
  while(n > 0 && off < (int)sizeof(result)-1) result[off++] = tmp[--n];
  if(off < (int)sizeof(result)-1){ result[off++] = ' '; result[off] = 0; }
  safestrcpy(result + off, "base_version=", sizeof(result) - off);
  off = strlen(result);
  v = base_version; n = 0;
  if(v == 0) tmp[n++] = '0';
  else { while(v > 0 && n < (int)sizeof(tmp)-1){ tmp[n++] = '0' + (v % 10); v /= 10; } }
  while(n > 0 && off < (int)sizeof(result)-1) result[off++] = tmp[--n];
  result[off] = 0;
  if(copyout(p->pagetable, uresult, result, strlen(result) + 1) < 0) return -1;
  return ret;
}

// 修改点 #2: lease_commit
uint64
sys_agent_lease_commit(void)
{
  uint64 lease_id, expected_version;
  argaddr(0, &lease_id); argaddr(1, &expected_version);
  return agent_proc_lease_commit(myproc(), lease_id, expected_version);
}

// 修改点 #2: lease_abort
uint64
sys_agent_lease_abort(void)
{
  uint64 lease_id; argaddr(0, &lease_id);
  return agent_proc_lease_abort(myproc(), lease_id);
}

// 修改点 #4/#2: agent_query_agent
uint64
sys_agent_query_agent(void)
{
  int role, capability, group; uint64 ubuf, len;
  argint(0, &role); argint(1, &capability); argint(2, &group);
  argaddr(3, &ubuf); argaddr(4, &len);
  return agent_proc_query_agent(myproc(), role, capability, group, ubuf, len);
}

// 修改点 #3: agent_role_set
uint64
sys_agent_role_set(void)
{
  int role;
  argint(0, &role);
  return agent_proc_role_set(myproc(), role);
}

uint64
sys_agent_context_verify(void)
{
  struct proc *p = myproc();
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  return agent_context_digest_verify(p);
}

uint64
sys_agent_cascade_kill(void)
{
  int reason;
  argint(0, &reason);
  if(reason != AGENT_CASCADE_PLANNER_EXIT &&
     reason != AGENT_CASCADE_EXPLICIT &&
     reason != AGENT_CASCADE_FAILURE)
    return AGENT_TOOL_ERR_BAD_PARAM;
  return agent_cascade_terminate(myproc(), reason, 0);
}
