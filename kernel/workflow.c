// Agent 工作流级量化指标。
//
// 指标按 workflow_id 和 leader generation 聚合，使 Planner 与所有 Worker
// 的 Tool Call、AgentFS 查询和事件等待能够形成同一份可追溯统计。

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "agent.h"

extern struct agent_global_state agent_global;

static struct agent_workflow *
workflow_find_locked(struct proc *p)
{
  if(p == 0 || p->workflow_id == 0 ||
     p->workflow_leader_generation == 0)
    return 0;

  for(int i = 0; i < AGENT_WORKFLOW_MAX; i++){
    struct agent_workflow *wf = &agent_global.workflows[i];

    if(wf->used && wf->workflow_id == p->workflow_id &&
       wf->leader_generation == p->workflow_leader_generation)
      return wf;
  }
  return 0;
}

void
agent_workflow_metric_syscall(struct proc *p)
{
  struct agent_workflow *wf;

  acquire(&agent_global.lock);
  if((wf = workflow_find_locked(p)) != 0)
    wf->metrics.syscalls++;
  release(&agent_global.lock);
}

void
agent_workflow_metric_tool_call(struct proc *p, int query_file)
{
  struct agent_workflow *wf;

  acquire(&agent_global.lock);
  if((wf = workflow_find_locked(p)) != 0){
    wf->metrics.tool_calls++;
    if(query_file)
      wf->metrics.query_file_calls++;
  }
  release(&agent_global.lock);
}

void
agent_workflow_metric_query(struct proc *p, uint64 files_scanned,
                            uint64 index_scanned, int cache_hit)
{
  struct agent_workflow *wf;

  acquire(&agent_global.lock);
  if((wf = workflow_find_locked(p)) != 0){
    wf->metrics.files_scanned += files_scanned;
    wf->metrics.index_scanned += index_scanned;
    if(cache_hit){
      wf->metrics.cache_hits++;
      // A valid cache hit proves the normalized query was already executed.
      wf->metrics.duplicate_queries++;
    } else {
      wf->metrics.cache_misses++;
    }
  }
  release(&agent_global.lock);
}

void
agent_workflow_metric_file_read(struct proc *p, uint64 bytes_read)
{
  struct agent_workflow *wf;

  acquire(&agent_global.lock);
  if((wf = workflow_find_locked(p)) != 0){
    wf->metrics.files_read++;
    wf->metrics.bytes_read += bytes_read;
  }
  release(&agent_global.lock);
}

void
agent_workflow_metric_wait(struct proc *p, uint64 wait_ticks,
                           int message_received)
{
  struct agent_workflow *wf;

  acquire(&agent_global.lock);
  if((wf = workflow_find_locked(p)) != 0){
    wf->metrics.wait_calls++;
    wf->metrics.wait_ticks += wait_ticks;
    if(message_received)
      wf->metrics.messages_received++;
  }
  release(&agent_global.lock);
}

int
agent_workflow_metrics_get(struct proc *p,
                           struct agent_workflow_metrics *metrics)
{
  struct agent_workflow *wf;

  if(p == 0 || metrics == 0 || p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;

  acquire(&agent_global.lock);
  wf = workflow_find_locked(p);
  if(wf == 0){
    release(&agent_global.lock);
    return AGENT_TOOL_ERR_SERVICE_GONE;
  }
  *metrics = wf->metrics;
  release(&agent_global.lock);
  return AGENT_TOOL_OK;
}
