// Agent Loop 内核运行机制。
//
// 这个文件集中实现任务五的运行时能力，包括：
// 心跳触发、消息/文件修改事件唤醒、Agent Loop 生命周期等待逻辑，
// 以及面向多 Agent 的调度评分与 tick 驱动唤醒。

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
#define AGENT_MAX_PRIORITY (10)
#define AGENT_AGING_DIVISOR (5)
#define AGENT_AGING_MAX (20)

extern struct proc proc[NPROC];

static uint64 agent_now_safe(void);
static int agent_event_weight(int pending_events);

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
agent_signal_event_locked(struct proc *p, int event, uint64 now)
{
  p->pending_events |= event;
  p->last_wakeup_reason = p->pending_events;
  p->wakeup_tick = now;
  if(p->agent_sched_boost < 2)
    p->agent_sched_boost = 2;
  if(p->state == SLEEPING && p->chan == p)
    p->state = RUNNABLE;
}

void
agent_signal_filemod(void)
{
  uint64 now = agent_now_safe();

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state != UNUSED &&
       p->agent_type != AGENT_TYPE_NORMAL &&
       p->loop_state != AGENT_LOOP_DONE &&
       (p->watch_mask & AGENT_WATCH_FILEMOD)){
      agent_signal_event_locked(p, AGENT_EVENT_FILEMOD, now);
    }
    release(&p->lock);
  }
}

int
agent_proc_heartbeat_set(struct proc *p, int interval)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(interval <= 0)
    return -1;

  acquire(&p->lock);
  p->heartbeat_interval = interval;
  p->heartbeat_deadline = agent_now_safe() + interval + 1;
  p->pending_events &= ~AGENT_EVENT_HEARTBEAT;
  if(p->last_wakeup_reason == AGENT_EVENT_HEARTBEAT)
    p->last_wakeup_reason = AGENT_EVENT_NONE;
  if(p->loop_state != AGENT_LOOP_DONE)
    p->loop_state = AGENT_LOOP_READY;
  release(&p->lock);
  return 0;
}

int
agent_proc_heartbeat_stop(struct proc *p)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;

  acquire(&p->lock);
  p->heartbeat_interval = 0;
  p->heartbeat_deadline = 0;
  p->pending_events &= ~AGENT_EVENT_HEARTBEAT;
  if(p->last_wakeup_reason == AGENT_EVENT_HEARTBEAT)
    p->last_wakeup_reason = AGENT_EVENT_NONE;
  release(&p->lock);
  return 0;
}

int
agent_proc_priority_set(struct proc *p, int priority)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(priority < 0 || priority > AGENT_MAX_PRIORITY)
    return -1;

  acquire(&p->lock);
  p->agent_priority = priority;
  if(priority >= AGENT_SCHED_PRIORITY_MIN &&
     priority <= AGENT_SCHED_PRIORITY_MAX)
    p->agent_sched_priority = priority;
  release(&p->lock);
  return 0;
}

int
agent_proc_watch(struct proc *p, int mask)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(mask & ~(AGENT_WATCH_MESSAGE | AGENT_WATCH_FILEMOD))
    return -1;

  acquire(&p->lock);
  p->watch_mask |= mask;
  if(p->loop_state != AGENT_LOOP_DONE)
    p->loop_state = AGENT_LOOP_READY;
  release(&p->lock);
  return 0;
}

int
agent_proc_unwatch(struct proc *p, int mask)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;

  acquire(&p->lock);
  if(mask == 0)
    p->watch_mask = 0;
  else
    p->watch_mask &= ~mask;
  if(!(p->watch_mask & AGENT_WATCH_MESSAGE))
    p->pending_events &= ~AGENT_EVENT_MESSAGE;
  if(!(p->watch_mask & AGENT_WATCH_FILEMOD)){
    p->pending_events &= ~AGENT_EVENT_FILEMOD;
    p->agent_watch_dev = 0;
    p->agent_watch_inum = 0;
    p->agent_watch_path[0] = 0;
  }
  release(&p->lock);
  return 0;
}

int
agent_proc_watch_file(struct proc *p, uint64 upath)
{
  char path[AGENT_MESSAGE_MAX];
  struct inode *ip;
  uint dev;
  uint inum;

  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(copyinstr(p->pagetable, path, upath, sizeof(path)) < 0)
    return -1;

  begin_op();
  ip = namei(path);
  if(ip == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_FILE){
    iunlockput(ip);
    end_op();
    return -1;
  }
  dev = ip->dev;
  inum = ip->inum;
  iunlockput(ip);
  end_op();

  acquire(&p->lock);
  p->watch_mask |= AGENT_WATCH_FILEMOD;
  p->agent_watch_dev = dev;
  p->agent_watch_inum = inum;
  safestrcpy(p->agent_watch_path, path, sizeof(p->agent_watch_path));
  if(p->loop_state != AGENT_LOOP_DONE)
    p->loop_state = AGENT_LOOP_READY;
  release(&p->lock);
  return 0;
}

int
agent_proc_sched_set(struct proc *p, int priority, int quota)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(priority < AGENT_SCHED_PRIORITY_MIN || priority > AGENT_SCHED_PRIORITY_MAX)
    return -1;
  if(quota < AGENT_SCHED_QUOTA_MIN || quota > AGENT_SCHED_QUOTA_MAX)
    return -1;

  acquire(&p->lock);
  p->agent_sched_priority = priority;
  p->agent_sched_quota = quota;
  if(p->agent_sched_budget > quota)
    p->agent_sched_budget = quota;
  if(p->agent_sched_budget <= 0)
    p->agent_sched_budget = quota;
  release(&p->lock);
  return 0;
}

int
agent_proc_wait(struct proc *p, int continue_loop, uint64 uevent)
{
  struct agent_wait_event event;
  int reason;

  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;

  if(continue_loop == 0){
    acquire(&p->lock);
    p->loop_state = AGENT_LOOP_DONE;
    p->heartbeat_interval = 0;
    p->heartbeat_deadline = 0;
    p->watch_mask = 0;
    p->pending_events = 0;
    p->last_wakeup_reason = AGENT_EVENT_NONE;
    p->agent_message[0] = 0;
    p->agent_watch_dev = 0;
    p->agent_watch_inum = 0;
    p->agent_watch_path[0] = 0;
    p->agent_sched_boost = 0;
    release(&p->lock);
    return 0;
  }

  memset(&event, 0, sizeof(event));
  acquire(&p->lock);
  p->loop_state = AGENT_LOOP_WAITING;
  for(;;){
    reason = p->pending_events;
    if(reason != AGENT_EVENT_NONE){
      event.reason = reason;
      event.tick = p->wakeup_tick ? p->wakeup_tick : agent_now_safe();
      if(reason & AGENT_EVENT_MESSAGE)
        safestrcpy(event.message, p->agent_message, sizeof(event.message));
      if(reason & AGENT_EVENT_FILEMOD)
        safestrcpy(event.file, p->agent_watch_path, sizeof(event.file));
      p->pending_events = 0;
      p->last_wakeup_reason = reason;
      p->agent_message[0] = 0;
      p->loop_state = AGENT_LOOP_READY;
      release(&p->lock);
      if(uevent != 0 &&
         copyout(p->pagetable, uevent, (char*)&event, sizeof(event)) < 0)
        return -1;
      return reason;
    }
    if(p->killed){
      p->loop_state = AGENT_LOOP_DONE;
      release(&p->lock);
      return -1;
    }
    p->chan = p;
    p->state = SLEEPING;
    sched();
    p->chan = 0;
  }
}

static int
agent_event_weight(int pending_events)
{
  if(pending_events & AGENT_EVENT_MESSAGE)
    return 30;
  if(pending_events & AGENT_EVENT_FILEMOD)
    return 20;
  if(pending_events & AGENT_EVENT_HEARTBEAT)
    return 10;
  return 0;
}

int
agent_schedule_score(struct proc *p, uint64 now)
{
  int priority = AGENT_DEFAULT_PRIORITY;
  int score;
  int aging = 0;

  if(p->state != RUNNABLE)
    return -1;
  if(p->runnable_since == 0)
    p->runnable_since = now ? now : 1;
  if(now > p->runnable_since)
    aging = (now - p->runnable_since) / AGENT_AGING_DIVISOR;
  if(aging > AGENT_AGING_MAX)
    aging = AGENT_AGING_MAX;

  if(p->agent_type != AGENT_TYPE_NORMAL){
    priority = p->agent_priority;
    if(p->agent_sched_priority > priority)
      priority = p->agent_sched_priority;
  }
  score = priority * 10 + aging;
  if(p->agent_type != AGENT_TYPE_NORMAL){
    score += agent_event_weight(p->pending_events);
    score += p->agent_sched_boost * 10;
  }
  return score;
}

void
agent_tick(uint64 now)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state != UNUSED &&
       p->agent_type != AGENT_TYPE_NORMAL &&
       p->loop_state != AGENT_LOOP_DONE &&
       p->heartbeat_interval > 0 &&
       p->heartbeat_deadline > 0 &&
       now >= p->heartbeat_deadline){
      agent_signal_event_locked(p, AGENT_EVENT_HEARTBEAT, now);
      p->heartbeat_deadline = now + p->heartbeat_interval;
    }
    release(&p->lock);
  }
}

void
agent_notify_file_modified(uint dev, uint inum)
{
  struct proc *p;
  uint64 now = agent_now_safe();

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state != UNUSED &&
       p->agent_type != AGENT_TYPE_NORMAL &&
       (p->watch_mask & AGENT_WATCH_FILEMOD) &&
       p->agent_watch_inum == inum &&
       p->agent_watch_dev == dev){
      agent_signal_event_locked(p, AGENT_EVENT_FILEMOD, now);
    }
    release(&p->lock);
  }
}
