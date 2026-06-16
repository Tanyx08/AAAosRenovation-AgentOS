#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "agent.h"

#define AGENT_DEFAULT_PRIORITY (5)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

extern struct proc proc[NPROC];

static uint64 agent_now_safe(void);

static int
agent_default_sched_priority(int type)
{
  if(type == AGENT_TYPE_PRIMARY)
    return 5;
  if(type == AGENT_TYPE_WORKER)
    return 3;
  return AGENT_SCHED_PRIORITY_MIN;
}

static int
agent_default_sched_quota(int type)
{
  if(type == AGENT_TYPE_PRIMARY)
    return 4;
  if(type == AGENT_TYPE_WORKER)
    return 2;
  return 0;
}

static uint64
agent_now_safe(void)
{
  uint64 now;

  acquire(&tickslock);
  now = ticks;
  release(&tickslock);
  return now;
}

void
agent_init_proc(struct proc *p)
{
  p->agent_type = AGENT_TYPE_NORMAL;
  p->heartbeat_interval = 0;
  p->resource_quota = AGENT_CONTEXT_REGION_SIZE -
                      sizeof(struct agent_context_header);
  p->loop_state = AGENT_LOOP_IDLE;
  p->context_region_start = 0;
  p->context_region_size = 0;
  p->context_path_len = 0;
  p->context_node_count = 0;
  p->context_dropped_nodes = 0;
  p->heartbeat_deadline = 0;
  p->wakeup_tick = 0;
  p->runnable_since = 0;
  memset(p->context_offsets, 0, sizeof(p->context_offsets));
  memset(p->context_lengths, 0, sizeof(p->context_lengths));
  p->watch_mask = 0;
  p->pending_events = 0;
  p->last_wakeup_reason = AGENT_EVENT_NONE;
  p->agent_priority = AGENT_DEFAULT_PRIORITY;
  p->agent_group = 0;
  memset(p->agent_message, 0, sizeof(p->agent_message));
  p->agent_watch_dev = 0;
  p->agent_watch_inum = 0;
  memset(p->agent_watch_path, 0, sizeof(p->agent_watch_path));
  p->agent_sched_priority = AGENT_SCHED_PRIORITY_MIN;
  p->agent_sched_quota = 0;
  p->agent_sched_budget = 0;
  p->agent_sched_boost = 0;
}

void
agent_after_fork(struct proc *dst, struct proc *src)
{
  dst->agent_type = src->agent_type;
  dst->heartbeat_interval = src->heartbeat_interval;
  dst->resource_quota = src->resource_quota;
  dst->loop_state = src->loop_state;
  dst->context_region_start = src->context_region_start;
  dst->context_region_size = src->context_region_size;
  dst->context_path_len = src->context_path_len;
  dst->context_node_count = src->context_node_count;
  dst->context_dropped_nodes = src->context_dropped_nodes;
  dst->heartbeat_deadline = src->heartbeat_deadline;
  dst->wakeup_tick = src->wakeup_tick;
  dst->runnable_since = 0;
  memmove(dst->context_offsets, src->context_offsets,
          sizeof(dst->context_offsets));
  memmove(dst->context_lengths, src->context_lengths,
          sizeof(dst->context_lengths));
  dst->watch_mask = src->watch_mask;
  dst->pending_events = src->pending_events;
  dst->last_wakeup_reason = src->last_wakeup_reason;
  dst->agent_priority = src->agent_priority;
  dst->agent_group = src->agent_group;
  memmove(dst->agent_message, src->agent_message, sizeof(dst->agent_message));
  dst->agent_watch_dev = src->agent_watch_dev;
  dst->agent_watch_inum = src->agent_watch_inum;
  memmove(dst->agent_watch_path, src->agent_watch_path,
          sizeof(dst->agent_watch_path));
  dst->agent_sched_priority = src->agent_sched_priority;
  dst->agent_sched_quota = src->agent_sched_quota;
  dst->agent_sched_budget = src->agent_sched_budget;
  dst->agent_sched_boost = src->agent_sched_boost;
}

uint64
agent_mark_current(int type, int heartbeat_interval, uint64 resource_quota)
{
  struct proc *p = myproc();
  uint64 start;
  uint64 end;

  if(type <= AGENT_TYPE_NORMAL)
    type = AGENT_TYPE_PRIMARY;
  if(p->context_region_start == 0){
    start = PGROUNDUP(p->sz);
    end = start + AGENT_CONTEXT_REGION_SIZE;
    if(uvmalloc(p->pagetable, p->sz, end, PTE_W) == 0)
      return -1;
    p->sz = end;
    p->context_region_start = start;
    p->context_region_size = AGENT_CONTEXT_REGION_SIZE;
  }
  p->agent_type = type;
  p->heartbeat_interval = heartbeat_interval;
  p->resource_quota = resource_quota;
  p->loop_state = AGENT_LOOP_READY;
  if(type == AGENT_TYPE_PRIMARY)
    p->agent_group = p->pid;
  else if(p->agent_group == 0)
    p->agent_group = p->pid;
  if(p->agent_priority <= 0)
    p->agent_priority = AGENT_DEFAULT_PRIORITY;
  p->heartbeat_deadline = heartbeat_interval > 0 ?
                          agent_now_safe() + heartbeat_interval : 0;
  p->wakeup_tick = 0;
  p->watch_mask = 0;
  p->pending_events = 0;
  p->last_wakeup_reason = AGENT_EVENT_NONE;
  p->agent_message[0] = 0;
  p->agent_watch_dev = 0;
  p->agent_watch_inum = 0;
  p->agent_watch_path[0] = 0;
  p->agent_sched_priority = agent_default_sched_priority(type);
  p->agent_sched_quota = agent_default_sched_quota(type);
  p->agent_sched_budget = p->agent_sched_quota;
  p->agent_sched_boost = 0;
  agent_context_clear(p);
  return p->context_region_start;
}

int
agent_get_info(struct proc *p, struct agent_info *info)
{
  info->context_start = p->context_region_start;
  info->context_size = p->context_region_size;
  info->agent_type = p->agent_type;
  info->heartbeat_interval = p->heartbeat_interval;
  info->resource_quota = p->resource_quota;
  info->loop_state = p->loop_state;
  info->context_path_len = p->context_path_len;
  info->context_node_count = p->context_node_count;
  info->dropped_nodes = p->context_dropped_nodes;
  info->agent_priority = p->agent_priority;
  info->agent_group = p->agent_group;
  info->sched_priority = p->agent_sched_priority;
  info->sched_quota = p->agent_sched_quota;
  info->sched_budget = p->agent_sched_budget;
  return 0;
}
