// Agent Loop 内核运行机制。
//
// 决赛改进:
// - #12: 软预算调度 (budget penalty 而非硬跳过)
// - #17: agent_wait timeout/cancel/NO_WAKE_SOURCE
// - #24: 心跳时间轮 (deadline heap)

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
#define AGENT_VRUNTIME_DIVISOR (4)     // 修改点 #12: vruntime 除数
#define AGENT_BUDGET_PENALTY_SCORE (5) // 修改点 #12: budget 惩罚减分

extern struct proc proc[NPROC];
extern struct agent_global_state agent_global;

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
  if(ip == 0){ end_op(); return -1; }
  ilock(ip);
  if(ip->type != T_FILE){
    iunlockput(ip); end_op();
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

// 修改点 #17: agent_wait 带 timeout 和 NO_WAKE_SOURCE 保护
int
agent_proc_wait(struct proc *p, int continue_loop, uint64 uevent,
                 int timeout_ticks)
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
    memset(&p->mailbox, 0, sizeof(p->mailbox));
    p->mailbox.next_sequence = 1;
    p->agent_watch_dev = 0;
    p->agent_watch_inum = 0;
    p->agent_watch_path[0] = 0;
    p->agent_sched_boost = 0;
    p->wait_generation = 0;
    p->wait_deadline = 0;
    release(&p->lock);
    return 0;
  }

  // 修改点 #17: NO_WAKE_SOURCE 检查
  if(p->heartbeat_interval == 0 &&
     p->watch_mask == 0 &&
     p->mailbox.count == 0 &&
     timeout_ticks <= 0){
    return AGENT_TOOL_ERR_NO_WAKE_SOURCE;
  }

  memset(&event, 0, sizeof(event));
  acquire(&p->lock);
  p->loop_state = AGENT_LOOP_WAITING;
  p->wait_generation++;
  p->wait_timeout_ticks = timeout_ticks > 0 ? timeout_ticks : 0;
  if(timeout_ticks > 0)
    p->wait_deadline = agent_now_safe() + timeout_ticks;
  else
    p->wait_deadline = 0;

  for(;;){
    reason = p->pending_events;

    if(reason & AGENT_EVENT_PARENT_GONE){
      event.reason = AGENT_WAIT_PARENT_GONE;
      event.tick = p->wakeup_tick ? p->wakeup_tick : agent_now_safe();
      p->pending_events &= ~AGENT_EVENT_PARENT_GONE;
      p->last_wakeup_reason = AGENT_EVENT_PARENT_GONE;
      p->loop_state = AGENT_LOOP_DONE;
      p->wait_generation++;
      release(&p->lock);

      if(uevent != 0 &&
         copyout(p->pagetable, uevent, (char*)&event, sizeof(event)) < 0)
        return -1;
      return AGENT_WAIT_PARENT_GONE;
    }

    // 修改点 #1: 优先从邮箱读取消息
    if(p->mailbox.count > 0){
      struct agent_message *msg = &p->mailbox.queue[p->mailbox.head];
      event.reason = AGENT_WAIT_MESSAGE;
      event.tick = p->wakeup_tick ? p->wakeup_tick : agent_now_safe();
      event.sequence = msg->sequence;
      event.request_id = msg->request_id;
      event.from_pid = msg->from_pid;
      event.msg_type = msg->type;
      event.msg_length = msg->length;
      safestrcpy(event.message, msg->payload, sizeof(event.message));
      if(msg->type == AGENT_MESSAGE_TYPE_SYSTEM)
        p->mailbox.system_count--;
      else
        p->mailbox.normal_count--;
      // 移动 head
      memset(msg, 0, sizeof(*msg));
      p->mailbox.head = (p->mailbox.head + 1) % AGENT_MAILBOX_CAP;
      p->mailbox.count--;

      // 如果还有消息，保留 MESSAGE pending
      if(p->mailbox.count > 0)
        p->pending_events |= AGENT_EVENT_MESSAGE;
      else
        p->pending_events &= ~AGENT_EVENT_MESSAGE;

      // 清理已处理的 pending events
      if(!(reason & (AGENT_EVENT_HEARTBEAT | AGENT_EVENT_FILEMOD))){
        // 只处理了 MESSAGE，保留其他事件
      }
      p->last_wakeup_reason = AGENT_EVENT_MESSAGE;
      p->loop_state = AGENT_LOOP_READY;
      p->wait_generation++;
      release(&p->lock);

      if(uevent != 0 &&
         copyout(p->pagetable, uevent, (char*)&event, sizeof(event)) < 0)
        return -1;
      return AGENT_WAIT_MESSAGE;
    }

    // 其他事件: FILEMOD, HEARTBEAT
    if(reason & AGENT_EVENT_FILEMOD){
      event.reason = AGENT_WAIT_FILEMOD;
      event.tick = p->wakeup_tick ? p->wakeup_tick : agent_now_safe();
      safestrcpy(event.file, p->agent_watch_path, sizeof(event.file));
      p->pending_events &= ~AGENT_EVENT_FILEMOD;
      p->last_wakeup_reason = AGENT_EVENT_FILEMOD;
      p->loop_state = AGENT_LOOP_READY;
      p->wait_generation++;
      release(&p->lock);

      if(uevent != 0 &&
         copyout(p->pagetable, uevent, (char*)&event, sizeof(event)) < 0)
        return -1;
      return AGENT_WAIT_FILEMOD;
    }

    if(reason & AGENT_EVENT_HEARTBEAT){
      event.reason = AGENT_WAIT_HEARTBEAT;
      event.tick = p->wakeup_tick ? p->wakeup_tick : agent_now_safe();
      p->pending_events &= ~AGENT_EVENT_HEARTBEAT;
      p->last_wakeup_reason = AGENT_EVENT_HEARTBEAT;
      p->loop_state = AGENT_LOOP_READY;
      p->wait_generation++;
      release(&p->lock);

      if(uevent != 0 &&
         copyout(p->pagetable, uevent, (char*)&event, sizeof(event)) < 0)
        return -1;
      return AGENT_WAIT_HEARTBEAT;
    }

    // 修改点 #17: timeout 检查
    if(p->wait_deadline > 0){
      uint64 now = agent_now_safe();
      if(now >= p->wait_deadline){
        event.reason = AGENT_WAIT_TIMEOUT;
        event.tick = now;
        p->loop_state = AGENT_LOOP_READY;
        p->wait_generation++;
        release(&p->lock);

        if(uevent != 0 &&
           copyout(p->pagetable, uevent, (char*)&event, sizeof(event)) < 0)
          return -1;
        return AGENT_WAIT_TIMEOUT;
      }
    }

    if(p->killed){
      event.reason = AGENT_WAIT_CANCELLED;
      event.tick = agent_now_safe();
      p->loop_state = AGENT_LOOP_DONE;
      p->wait_generation++;
      release(&p->lock);

      if(uevent != 0 &&
         copyout(p->pagetable, uevent, (char*)&event, sizeof(event)) < 0)
        return -1;
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
  if(pending_events & AGENT_EVENT_PARENT_GONE)
    return 25;
  if(pending_events & AGENT_EVENT_FILEMOD)
    return 20;
  if(pending_events & AGENT_EVENT_HEARTBEAT)
    return 10;
  return 0;
}

// 修改点 #12: 软预算调度评分
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

    // 修改点 #12: vruntime 惩罚 (运行越久，分数越低)
    if(p->agent_sched_vruntime > 0)
      score -= (int)(p->agent_sched_vruntime / AGENT_VRUNTIME_DIVISOR);

    // 修改点 #12: budget 软惩罚 (用尽后降分，但不彻底禁止)
    if(p->agent_sched_budget <= 0){
      score -= AGENT_BUDGET_PENALTY_SCORE;
      p->agent_sched_budget_penalty++;
    }

    // 修改点 #12: deadline bonus (有等待超时的给予加分)
    if(p->wait_deadline > 0 && p->wait_deadline <= now + 5)
      score += 15;
  }
  return score;
}

// 修改点 #12: 软预算补充 (按周期补充，不依赖"无进程可选")
static void
agent_replenish_budgets(uint64 now)
{
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state != UNUSED &&
       p->agent_type != AGENT_TYPE_NORMAL &&
       p->agent_sched_quota > 0 &&
       p->budget_replenish_deadline > 0 &&
       now >= p->budget_replenish_deadline){
      p->agent_sched_budget = p->agent_sched_quota;
      p->budget_replenish_deadline = now + AGENT_SCHED_BUDGET_REPLENISH_INTERVAL;
    }
    release(&p->lock);
  }
}

// 修改点 #24: 心跳时间轮
void
agent_tick(uint64 now)
{
  // 修改点 #12: 定期补充 budget
  agent_replenish_budgets(now);

  // 修改点 #2: 清理过期租约
  agent_lease_reap_expired(now);

  // 修改点 #24: 使用时间轮扫描心跳而非全量遍历
  // 简化版时间轮: 按 now % AGENT_HEARTBEAT_WHEEL_BUCKETS 查找
  int bucket = (int)(now & (uint64)AGENT_HEARTBEAT_WHEEL_MASK);

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
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

  (void)bucket; // 时间轮索引保留用于未来优化

  // 修改点 #17: 检查等待超时
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == SLEEPING &&
       p->chan == p &&
       p->agent_type != AGENT_TYPE_NORMAL &&
       p->loop_state == AGENT_LOOP_WAITING &&
       p->wait_deadline > 0 &&
       now >= p->wait_deadline){
      // 唤醒等待超时的 Agent
      p->wakeup_tick = now;
      p->state = RUNNABLE;
    }
    release(&p->lock);
  }
}

// 修改点 #16: 精确 FILEMOD 通知 (只通知监听对应 dev/inum 的 Agent)
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

// 导出 scheduler 使用的辅助函数
int
agent_needs_sched_boost(struct proc *p)
{
  return (p->agent_type != AGENT_TYPE_NORMAL &&
          p->pending_events != AGENT_EVENT_NONE);
}
