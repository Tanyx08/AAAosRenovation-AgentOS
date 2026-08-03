// Agent 核心管理逻辑。
//
// 决赛改进:
// - #1: FIFO 有界邮箱替换单消息槽
// - #3: capability 权限模型集成
// - #4: 身份代次 (identity_generation) 防 PID 复用
// - #6: 内核可信 Context 摘要
// - #7: 固定 Context ABI + guard page
// - #8: Context 复用标记
// - #18: agent_spawn 级联创建
// - #19: 统一日志与 audit

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
#define AGENT_GUARD_PAGE_SIZE (4096)  // 修改点 #7: Context guard page

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

extern struct proc proc[NPROC];

// 全局 Agent 运行时状态
struct agent_global_state agent_global;

static uint64 agent_now_safe(void);

void
agent_global_init(void)
{
  memset(&agent_global, 0, sizeof(agent_global));
  initlock(&agent_global.lock, "agent_global");
  agent_global.ready = 1;
}

static uint64
agent_next_identity(void)
{
  uint64 generation;

  acquire(&agent_global.lock);
  generation = ++agent_global.next_identity_generation;
  if(generation == 0)
    generation = ++agent_global.next_identity_generation;
  release(&agent_global.lock);
  return generation;
}

static struct agent_workflow*
agent_workflow_find_locked(uint64 workflow_id, uint64 leader_generation)
{
  for(int i = 0; i < AGENT_WORKFLOW_MAX; i++){
    if(agent_global.workflows[i].used &&
       agent_global.workflows[i].workflow_id == workflow_id &&
       agent_global.workflows[i].leader_generation == leader_generation)
      return &agent_global.workflows[i];
  }
  return 0;
}

static int
agent_workflow_create_locked(struct proc *p)
{
  struct agent_workflow *wf;

  wf = agent_workflow_find_locked(p->workflow_id,
                                  p->workflow_leader_generation);
  if(wf != 0){
    if(wf->state == AGENT_WORKFLOW_TERMINATING)
      return AGENT_TOOL_ERR_BUSY;
    wf->leader_pid = p->workflow_leader_pid;
    wf->member_count++;
    return AGENT_TOOL_OK;
  }

  for(int i = 0; i < AGENT_WORKFLOW_MAX; i++){
    wf = &agent_global.workflows[i];
    if(!wf->used){
      memset(wf, 0, sizeof(*wf));
      wf->used = 1;
      wf->workflow_id = p->workflow_id;
      wf->leader_pid = p->workflow_leader_pid;
      wf->leader_generation = p->workflow_leader_generation;
      wf->state = AGENT_WORKFLOW_ACTIVE;
      wf->member_count = 1;
      return AGENT_TOOL_OK;
    }
  }
  return AGENT_TOOL_ERR_NO_SPACE;
}

static int
agent_workflow_join_locked(struct proc *p)
{
  struct agent_workflow *wf;

  if(p->workflow_id == 0 || p->workflow_leader_generation == 0)
    return AGENT_TOOL_OK;
  wf = agent_workflow_find_locked(p->workflow_id,
                                  p->workflow_leader_generation);
  if(wf == 0)
    return AGENT_TOOL_ERR_SERVICE_GONE;
  if(wf->state == AGENT_WORKFLOW_TERMINATING)
    return AGENT_TOOL_ERR_BUSY;
  if(wf->member_count >= AGENT_WORKFLOW_MEMBER_MAX)
    return AGENT_TOOL_ERR_NO_SPACE;
  wf->member_count++;
  return AGENT_TOOL_OK;
}

static int
agent_workflow_begin_terminate_locked(uint64 workflow_id,
                                      uint64 leader_generation)
{
  struct agent_workflow *wf =
    agent_workflow_find_locked(workflow_id, leader_generation);

  if(wf == 0)
    return AGENT_TOOL_ERR_SERVICE_GONE;
  if(wf->state == AGENT_WORKFLOW_TERMINATING)
    return AGENT_TOOL_OK;
  if(wf->state != AGENT_WORKFLOW_ACTIVE)
    return AGENT_TOOL_ERR_SERVICE_GONE;
  wf->state = AGENT_WORKFLOW_TERMINATING;
  wf->terminate_tick = agent_now_safe();
  return AGENT_TOOL_OK;
}

int
agent_workflow_leave(struct proc *p)
{
  struct agent_workflow *wf;

  if(!p->workflow_member_registered ||
     p->workflow_id == 0 || p->workflow_leader_generation == 0)
    return 0;

  acquire(&agent_global.lock);
  wf = agent_workflow_find_locked(p->workflow_id,
                                  p->workflow_leader_generation);
  if(wf != 0){
    if(wf->member_count > 0)
      wf->member_count--;
    if(wf->member_count <= 0){
      wf->state = AGENT_WORKFLOW_DEAD;
      memset(wf, 0, sizeof(*wf));
    }
  }
  release(&agent_global.lock);
  p->workflow_member_registered = 0;
  return 0;
}

int
agent_cascade_terminate(struct proc *leader, int reason, int include_leader)
{
  uint64 workflow_id;
  uint64 leader_generation;
  int leader_pid;
  int ret;
  uint64 now = agent_now_safe();

  if(leader == 0 || leader->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(!leader->is_workflow_leader &&
     !(leader->agent_capabilities & AGENT_CAP_WORKFLOW_CTRL))
    return AGENT_TOOL_ERR_PERMISSION;
  if(leader->workflow_id == 0 || leader->workflow_leader_generation == 0)
    return AGENT_TOOL_ERR_BAD_PARAM;

  workflow_id = leader->workflow_id;
  leader_generation = leader->workflow_leader_generation;
  leader_pid = leader->workflow_leader_pid;

  acquire(&agent_global.lock);
  ret = agent_workflow_begin_terminate_locked(workflow_id, leader_generation);
  release(&agent_global.lock);
  if(ret != AGENT_TOOL_OK && ret != AGENT_TOOL_ERR_SERVICE_GONE)
    return ret;

  printf("[Agent-Lifecycle] workflow=%d leader=%d state=TERMINATING reason=%d\n",
         (int)workflow_id, leader_pid, reason);

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state != UNUSED &&
       p->agent_type != AGENT_TYPE_NORMAL &&
       p->workflow_id == workflow_id &&
       p->workflow_leader_generation == leader_generation &&
       (include_leader || p != leader)){
      p->killed = 1;
      p->loop_state = AGENT_LOOP_DONE;
      p->heartbeat_interval = 0;
      p->heartbeat_deadline = 0;
      p->watch_mask = 0;
      p->pending_events |= AGENT_EVENT_PARENT_GONE;
      p->last_wakeup_reason = AGENT_EVENT_PARENT_GONE;
      p->wakeup_tick = now;
      p->wait_deadline = 0;
      p->agent_watch_dev = 0;
      p->agent_watch_inum = 0;
      p->agent_watch_path[0] = 0;
      if(p->state == SLEEPING && p->chan == p)
        p->state = RUNNABLE;
      else if(p->state == SLEEPING)
        p->state = RUNNABLE;
      printf("[Agent-Lifecycle] cascade target=%d role=%d status=KILLED\n",
             p->pid, p->agent_role);
    }
    release(&p->lock);
  }
  return AGENT_TOOL_OK;
}

// ---- 角色→默认 capability 映射 (修改点 #3) ----
uint64 agent_role_default_caps(int role) {
  switch(role) {
    case 0: // Planner
      return AGENT_CAP_QUERY_PROCESS | AGENT_CAP_QUERY_FILE |
             AGENT_CAP_SEND_MESSAGE | AGENT_CAP_WORKFLOW_CTRL |
             AGENT_CAP_SCHED_CONFIG | AGENT_CAP_AUDIT_READ;
    case 1: // Retriever
      return AGENT_CAP_QUERY_FILE | AGENT_CAP_READ_FILE |
             AGENT_CAP_SEND_MESSAGE;
    case 2: // Patch
      return AGENT_CAP_READ_FILE | AGENT_CAP_PATCH_FILE |
             AGENT_CAP_LEASE_ACQUIRE | AGENT_CAP_SEND_MESSAGE;
    case 3: // Test
      return AGENT_CAP_QUERY_FILE | AGENT_CAP_READ_FILE |
             AGENT_CAP_SEND_MESSAGE;
    case 4: // Reviewer
      return AGENT_CAP_READ_FILE | AGENT_CAP_WATCH_FILE |
             AGENT_CAP_SEND_MESSAGE | AGENT_CAP_QUERY_FILE;
    case 5: // Tool-Service
      return AGENT_CAP_REGISTER_TOOL | AGENT_CAP_SEND_MESSAGE;
    default:
      return 0;
  }
}

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

// ---- 简单 hash 函数 (修改点 #6) ----
static uint64
agent_hash_bytes(const char *data, int len)
{
  uint64 h = 14695981039346656037ULL;
  for(int i = 0; i < len && data[i]; i++){
    h ^= (unsigned char)data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

// ---- 内核可信摘要管理 (修改点 #6) ----
void
agent_context_digest_push(struct proc *p, uint64 sequence, uint64 request_id,
                           const char *request, const char *result, int status)
{
  struct agent_context_digest *d;
  uint64 prev_hash = 0;

  if(p->context_digest_count > 0){
    int prev = p->context_digest_head - 1;
    if(prev < 0) prev = AGENT_CONTEXT_DIGEST_MAX - 1;
    prev_hash = p->context_digests[prev].current_hash;
  }

  d = &p->context_digests[p->context_digest_head];
  d->sequence = sequence;
  d->request_id = request_id;
  d->request_hash = agent_hash_bytes(request, strlen(request));
  d->result_hash = agent_hash_bytes(result, strlen(result));
  d->previous_hash = prev_hash;
  d->status = status;

  // 当前摘要包含前一条摘要的 hash，形成不可断开的链。
  uint64 h = 14695981039346656037ULL;
  h ^= sequence;
  h *= 1099511628211ULL;
  h ^= request_id;
  h *= 1099511628211ULL;
  h ^= d->request_hash;
  h *= 1099511628211ULL;
  h ^= d->result_hash;
  h *= 1099511628211ULL;
  h ^= prev_hash;
  h *= 1099511628211ULL;
  h ^= (uint64)(uint32)status;
  h *= 1099511628211ULL;
  d->current_hash = h;

  p->context_digest_head = (p->context_digest_head + 1) % AGENT_CONTEXT_DIGEST_MAX;
  if(p->context_digest_count < AGENT_CONTEXT_DIGEST_MAX)
    p->context_digest_count++;
}

// ---- 修改点 #7: 内核可信摘要校验 ----
int
agent_context_digest_verify(struct proc *p)
{
  int first;
  uint64 expected_prev = 0;
  uint64 previous_sequence = 0;

  if(p->context_digest_count < 0 ||
     p->context_digest_count > AGENT_CONTEXT_DIGEST_MAX ||
     p->context_digest_head < 0 ||
     p->context_digest_head >= AGENT_CONTEXT_DIGEST_MAX)
    return -1;
  if(p->context_digest_count == 0)
    return 0;

  first = p->context_digest_head - p->context_digest_count;
  while(first < 0)
    first += AGENT_CONTEXT_DIGEST_MAX;

  for(int i = 0; i < p->context_digest_count; i++){
    struct agent_context_digest *d =
      &p->context_digests[(first + i) % AGENT_CONTEXT_DIGEST_MAX];
    uint64 h = 14695981039346656037ULL;

    if(i > 0 && d->previous_hash != expected_prev)
      return -1;
    if(i > 0 && d->sequence != previous_sequence + 1)
      return -1;
    h ^= d->sequence; h *= 1099511628211ULL;
    h ^= d->request_id; h *= 1099511628211ULL;
    h ^= d->request_hash; h *= 1099511628211ULL;
    h ^= d->result_hash; h *= 1099511628211ULL;
    h ^= d->previous_hash; h *= 1099511628211ULL;
    h ^= (uint64)(uint32)d->status; h *= 1099511628211ULL;
    if(h != d->current_hash)
      return -1;
    expected_prev = d->current_hash;
    previous_sequence = d->sequence;
  }
  return 0;
}

void
agent_init_proc(struct proc *p)
{
  p->agent_type = AGENT_TYPE_NORMAL;
  p->agent_role = AGENT_ROLE_UNSET;
  p->agent_role_locked = 0;
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
  p->agent_capabilities = 0;
  p->workflow_id = 0;
  p->workflow_leader_pid = 0;
  p->workflow_leader_generation = 0;
  p->agent_parent_pid = 0;
  p->agent_parent_generation = 0;
  p->is_workflow_leader = 0;
  p->workflow_member_registered = 0;
  p->identity_generation = 0;

  // 修改点 #1: 初始化邮箱
  memset(&p->mailbox, 0, sizeof(p->mailbox));
  p->mailbox.next_sequence = 1;

  p->agent_watch_dev = 0;
  p->agent_watch_inum = 0;
  memset(p->agent_watch_path, 0, sizeof(p->agent_watch_path));
  p->agent_sched_priority = AGENT_SCHED_PRIORITY_MIN;
  p->agent_sched_quota = 0;
  p->agent_sched_budget = 0;
  p->agent_sched_boost = 0;
  p->agent_sched_vruntime = 0;
  p->agent_sched_last_run = 0;
  p->agent_sched_budget_penalty = 0;

  p->context_generation = 1;
  p->context_first_sequence = 0;
  p->context_next_sequence = 1;

  memset(p->context_digests, 0, sizeof(p->context_digests));
  p->context_digest_head = 0;
  p->context_digest_count = 0;

  p->wait_generation = 0;
  p->wait_timeout_ticks = 0;
  p->wait_deadline = 0;

  p->last_context_query = 0;
  p->context_reuse_hit = 0;

  p->budget_replenish_deadline = 0;
}

// 修改点 #18: fork 继承规则文档化 + identity_generation 递增
void
agent_after_fork(struct proc *dst, struct proc *src)
{
  // Agent 身份继承（降权: fork 不继承 capability，需重新认证）
  dst->agent_type = src->agent_type;
  dst->agent_role = AGENT_ROLE_UNSET;
  dst->agent_role_locked = 0;
  dst->heartbeat_interval = 0;              // fork 后不继承心跳
  dst->resource_quota = src->resource_quota;
  dst->loop_state = AGENT_LOOP_IDLE;        // fork 后重置为 IDLE
  dst->context_region_start = src->context_region_start;
  dst->context_region_size = src->context_region_size;
  dst->context_path_len = 0;                // fork 不继承 Context 内容
  dst->context_node_count = 0;
  dst->context_dropped_nodes = 0;
  dst->heartbeat_deadline = 0;              // fork 不继承心跳
  dst->wakeup_tick = 0;
  dst->runnable_since = 0;
  memset(dst->context_offsets, 0, sizeof(dst->context_offsets));
  memset(dst->context_lengths, 0, sizeof(dst->context_lengths));
  dst->watch_mask = 0;                      // fork 不继承 watch
  dst->pending_events = 0;
  dst->last_wakeup_reason = AGENT_EVENT_NONE;
  dst->agent_priority = src->agent_priority;
  dst->agent_group = src->agent_group;
  dst->agent_capabilities = 0;              // fork 后 capability 清零（需重新认证）
  dst->workflow_id = src->workflow_id;
  dst->identity_generation = agent_next_identity();
  dst->workflow_leader_pid = src->workflow_leader_pid;
  dst->workflow_leader_generation = src->workflow_leader_generation;
  dst->agent_parent_pid = src->pid;
  dst->agent_parent_generation = src->identity_generation;
  dst->is_workflow_leader = 0;              // fork 子进程不能误当 workflow leader
  dst->workflow_member_registered = 0;
  if(dst->workflow_id != 0 && dst->workflow_leader_generation != 0){
    acquire(&agent_global.lock);
    if(agent_workflow_join_locked(dst) == AGENT_TOOL_OK)
      dst->workflow_member_registered = 1;
    else
      dst->killed = 1;
    release(&agent_global.lock);
  }

  // 邮箱不继承
  memset(&dst->mailbox, 0, sizeof(dst->mailbox));
  dst->mailbox.next_sequence = 1;

  dst->agent_watch_dev = 0;
  dst->agent_watch_inum = 0;
  memset(dst->agent_watch_path, 0, sizeof(dst->agent_watch_path));

  // 调度参数继承但 budget 清零
  dst->agent_sched_priority = src->agent_sched_priority;
  dst->agent_sched_quota = src->agent_sched_quota;
  dst->agent_sched_budget = 0;
  dst->agent_sched_boost = 0;
  dst->agent_sched_vruntime = 0;
  dst->agent_sched_last_run = 0;
  dst->agent_sched_budget_penalty = 0;

  dst->context_generation = src->context_generation + 1;
  dst->context_first_sequence = 0;
  dst->context_next_sequence = 1;

  memset(dst->context_digests, 0, sizeof(dst->context_digests));
  dst->context_digest_head = 0;
  dst->context_digest_count = 0;

  dst->wait_generation = 0;
  dst->wait_timeout_ticks = 0;
  dst->wait_deadline = 0;
  dst->budget_replenish_deadline = 0;

  dst->last_context_query = 0;
  dst->context_reuse_hit = 0;
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
    // 修改点 #7: Context 区域末尾增加 guard page
    start = PGROUNDUP(p->sz);
    end = start + AGENT_CONTEXT_REGION_SIZE + AGENT_GUARD_PAGE_SIZE;
    if(uvmalloc(p->pagetable, p->sz, end, PTE_W) == 0)
      return -1;
    // Context 末页保留在地址空间中，但清除 PTE_U，作为真正的 guard page。
    uvmclear(p->pagetable, start + AGENT_CONTEXT_REGION_SIZE);
    p->sz = end;
    p->context_region_start = start;
    p->context_region_size = AGENT_CONTEXT_REGION_SIZE;
  }
  p->agent_type = type;
  p->agent_role = type == AGENT_TYPE_PRIMARY ? AGENT_ROLE_PLANNER :
                                                AGENT_ROLE_UNSET;
  p->agent_role_locked = type == AGENT_TYPE_PRIMARY;
  p->heartbeat_interval = heartbeat_interval;
  p->resource_quota = resource_quota;
  p->loop_state = AGENT_LOOP_READY;
  if(p->identity_generation == 0)
    p->identity_generation = agent_next_identity();

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

  // 修改点 #1: 邮箱初始化
  memset(&p->mailbox, 0, sizeof(p->mailbox));
  p->mailbox.next_sequence = 1;

  p->agent_watch_dev = 0;
  p->agent_watch_inum = 0;
  p->agent_watch_path[0] = 0;
  p->agent_sched_priority = agent_default_sched_priority(type);
  p->agent_sched_quota = agent_default_sched_quota(type);
  p->agent_sched_budget = p->agent_sched_quota;
  p->agent_sched_boost = 0;
  p->agent_sched_vruntime = 0;
  p->agent_sched_last_run = 0;
  p->agent_sched_budget_penalty = 0;
  p->budget_replenish_deadline = 0;

  // 修改点 #3: 默认赋予基本 capability
  p->agent_capabilities = type == AGENT_TYPE_PRIMARY ? AGENT_CAP_ALL :
    (AGENT_CAP_QUERY_PROCESS | AGENT_CAP_QUERY_FILE |
     AGENT_CAP_READ_FILE | AGENT_CAP_SEND_MESSAGE);
  if(type == AGENT_TYPE_PRIMARY && p->workflow_member_registered)
    agent_workflow_leave(p);

  if(type == AGENT_TYPE_PRIMARY){
    p->workflow_id = (uint64)p->pid;
    p->workflow_leader_pid = p->pid;
    p->workflow_leader_generation = p->identity_generation;
    p->agent_parent_pid = 0;
    p->agent_parent_generation = 0;
    p->is_workflow_leader = 1;
    acquire(&agent_global.lock);
    if(!p->workflow_member_registered &&
       agent_workflow_create_locked(p) == AGENT_TOOL_OK)
      p->workflow_member_registered = 1;
    release(&agent_global.lock);
  } else {
    p->is_workflow_leader = 0;
    if(p->workflow_id == 0 || p->workflow_leader_generation == 0){
      p->workflow_id = (uint64)p->pid;
      p->workflow_leader_pid = p->pid;
      p->workflow_leader_generation = p->identity_generation;
      p->agent_parent_pid = 0;
      p->agent_parent_generation = 0;
      acquire(&agent_global.lock);
      if(!p->workflow_member_registered &&
         agent_workflow_create_locked(p) == AGENT_TOOL_OK)
        p->workflow_member_registered = 1;
      release(&agent_global.lock);
    } else if(!p->workflow_member_registered){
      acquire(&agent_global.lock);
      if(agent_workflow_join_locked(p) == AGENT_TOOL_OK)
        p->workflow_member_registered = 1;
      else {
        release(&agent_global.lock);
        return -1;
      }
      release(&agent_global.lock);
    }
  }

  // 修改点 #7: Context generation 初始化
  p->context_generation = 1;
  p->context_first_sequence = 0;
  p->context_next_sequence = 1;

  // 修改点 #6: 摘要初始化
  memset(p->context_digests, 0, sizeof(p->context_digests));
  p->context_digest_head = 0;
  p->context_digest_count = 0;

  // 修改点 #17: wait 参数初始化
  p->wait_generation = 0;
  p->wait_timeout_ticks = 0;
  p->wait_deadline = 0;

  // 修改点 #8: 复用标记初始化
  p->last_context_query = 0;
  p->context_reuse_hit = 0;

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
  info->agent_capabilities = p->agent_capabilities;
  info->mailbox_count = p->mailbox.count;
  info->mailbox_dropped = p->mailbox.dropped;
  return 0;
}

// 修改点 #3: 设置 capability
int
agent_proc_cap_set(struct proc *p, uint64 caps)
{
  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(caps & ~AGENT_CAP_ALL)
    return AGENT_TOOL_ERR_BAD_PARAM;
  acquire(&p->lock);
  // capability 只能削减；增加权限必须由受信任的创建/角色分配路径完成。
  if(caps & ~p->agent_capabilities){
    release(&p->lock);
    agent_audit_record(p, p->pid, "cap_set", 0,
                       AGENT_TOOL_ERR_PERMISSION, "capability escalation");
    return AGENT_TOOL_ERR_PERMISSION;
  }
  p->agent_capabilities = caps;
  release(&p->lock);
  return 0;
}

int
agent_proc_role_set(struct proc *p, int role)
{
  uint64 caps;

  if(p->agent_type == AGENT_TYPE_NORMAL)
    return AGENT_TOOL_ERR_NOT_AGENT;
  if(role < AGENT_ROLE_PLANNER || role > AGENT_ROLE_TOOL_SERVICE)
    return AGENT_TOOL_ERR_BAD_PARAM;
  caps = agent_role_default_caps(role);

  acquire(&p->lock);
  if(p->agent_role_locked){
    release(&p->lock);
    return AGENT_TOOL_ERR_PERMISSION;
  }
  p->agent_role = role;
  p->agent_capabilities = caps;
  p->agent_role_locked = 1;
  release(&p->lock);
  return AGENT_TOOL_OK;
}

// 修改点 #1: FIFO 消息发送
int
agent_send_message(struct proc *src, int target_pid, int msg_type,
                    const char *payload, uint64 request_id)
{
  struct proc *target = 0;
  uint64 now;
  int slot;

  if(payload == 0 ||
     (msg_type != AGENT_MESSAGE_TYPE_NORMAL &&
      msg_type != AGENT_MESSAGE_TYPE_SYSTEM))
    return AGENT_TOOL_ERR_BAD_PARAM;

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == target_pid && p->state != UNUSED &&
       p->agent_type != AGENT_TYPE_NORMAL){
      target = p;
      break;
    }
    release(&p->lock);
  }
  if(target == 0)
    return AGENT_TOOL_ERR_SERVICE_GONE;

  now = agent_now_safe();

  if((msg_type == AGENT_MESSAGE_TYPE_NORMAL &&
      target->mailbox.normal_count >=
        AGENT_MAILBOX_CAP - AGENT_MAILBOX_SYSTEM_SLOTS) ||
     target->mailbox.count >= AGENT_MAILBOX_CAP){
    target->mailbox.dropped++;
    if(msg_type == AGENT_MESSAGE_TYPE_SYSTEM)
      target->mailbox.system_dropped++;
    release(&target->lock);
    return AGENT_TOOL_ERR_BUSY;
  }

  slot = target->mailbox.tail;
  target->mailbox.queue[slot].from_pid = src ? src->pid : 0;
  target->mailbox.queue[slot].type = msg_type;
  target->mailbox.queue[slot].length = MIN(strlen(payload), AGENT_MESSAGE_MAX - 1);
  target->mailbox.queue[slot].sequence = target->mailbox.next_sequence++;
  target->mailbox.queue[slot].request_id = request_id;
  safestrcpy(target->mailbox.queue[slot].payload, payload,
             sizeof(target->mailbox.queue[slot].payload));
  target->mailbox.tail = (target->mailbox.tail + 1) % AGENT_MAILBOX_CAP;
  target->mailbox.count++;
  if(msg_type == AGENT_MESSAGE_TYPE_SYSTEM)
    target->mailbox.system_count++;
  else
    target->mailbox.normal_count++;

  agent_signal_event_locked(target, AGENT_EVENT_MESSAGE, now);
  release(&target->lock);
  return AGENT_TOOL_OK;
}

// 修改点 #4/#2: Agent 发现查询
int
agent_proc_query_agent(struct proc *p, int role, int capability, int group,
                   uint64 dst, uint64 len)
{
  char *buf;
  char *ptr;
  int left = AGENT_TOOL_RESULT_MAX;
  int count = 0;

  buf = kalloc();
  if(buf == 0) return -1;
  ptr = buf;
  memset(buf, 0, AGENT_TOOL_RESULT_MAX);

  buf_puts_s(&ptr, &left, "{agents=[");
  for(struct proc *pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->state == UNUSED || pp->agent_type == AGENT_TYPE_NORMAL)
      continue;
    if(role >= 0 && pp->agent_role != role)
      continue;
    if(capability >= 0 && !(pp->agent_capabilities & (1ULL << capability)))
      continue;
    if(group > 0 && pp->agent_group != group)
      continue;

    if(count > 0) buf_putc_s(&ptr, &left, ',');
    buf_puts_s(&ptr, &left, "{pid=");
    buf_putu_s(&ptr, &left, pp->pid);
    buf_puts_s(&ptr, &left, ",type=");
    buf_putu_s(&ptr, &left, pp->agent_type);
    buf_puts_s(&ptr, &left, ",role=");
    buf_putu_s(&ptr, &left, pp->agent_role);
    buf_puts_s(&ptr, &left, ",caps=");
    buf_putu_s(&ptr, &left, pp->agent_capabilities);
    buf_puts_s(&ptr, &left, ",group=");
    buf_putu_s(&ptr, &left, pp->agent_group);
    buf_puts_s(&ptr, &left, ",state=");
    buf_putu_s(&ptr, &left, pp->loop_state);
    buf_putc_s(&ptr, &left, '}');
    count++;
  }
  buf_puts_s(&ptr, &left, "],count=");
  buf_putu_s(&ptr, &left, count);
  buf_putc_s(&ptr, &left, '}');

  uint64 n = MIN((uint64)strlen(buf), len);
  if(copyout(p->pagetable, dst, buf, n) < 0){
    kfree(buf);
    return -1;
  }
  kfree(buf);
  return n;
}

// 修改点 #19: 统一日志
void
agent_trace(struct proc *p, const char *action, int status, const char *cause)
{
  uint64 now = agent_now_safe();
  printf("[TRACE] tick=%d span=0 req=0 agent=%d role=%d action=%s status=%d cause=%s\n",
         (int)now, p->pid, p->agent_role, action, status, cause);
}

void
agent_trace_span(struct proc *p, uint64 span_id, uint64 request_id,
                  const char *action, int status, const char *cause)
{
  uint64 now = agent_now_safe();
  printf("[TRACE] tick=%d span=%d req=%d agent=%d role=%d action=%s status=%d cause=%s\n",
         (int)now, (int)span_id, (int)request_id, p->pid, p->agent_role,
         action, status, cause);
}

void
agent_audit_record(struct proc *p, uint64 target, const char *action,
                    int decision, int status, const char *cause)
{
  uint64 now = agent_now_safe();
  printf("[AUDIT] tick=%d actor=%d.%d target=%d caps=0x%x action=%s decision=%s status=%d cause=%s\n",
         (int)now, p->pid, (int)p->identity_generation, (int)target,
         (int)p->agent_capabilities, action,
         decision ? "allow" : "deny", status, cause);
}

// 修改点 #2: 文件编辑租约
int
agent_proc_lease_begin(struct proc *p, const char *path, uint64 *lease_id,
                   uint64 *base_version)
{
  struct inode *ip;
  int slot = -1;
  uint64 now;

  if(!(p->agent_capabilities & AGENT_CAP_LEASE_ACQUIRE) &&
     !(p->agent_capabilities & AGENT_CAP_PATCH_FILE))
    return AGENT_TOOL_ERR_PERMISSION;

  begin_op();
  ip = namei((char*)path);
  if(ip == 0){ end_op(); return AGENT_TOOL_ERR_BAD_PARAM; }
  ilock(ip);
  if(ip->type != T_FILE){
    iunlockput(ip); end_op();
    return AGENT_TOOL_ERR_BAD_PARAM;
  }

  now = agent_now_safe();
  acquire(&agent_global.lock);

  for(int i = 0; i < AGENT_LEASE_MAX; i++){
    if(agent_global.leases[i].used &&
       agent_global.leases[i].expiry_tick <= now)
      memset(&agent_global.leases[i], 0, sizeof(agent_global.leases[i]));
  }

  // 检查同一 inode 是否已被租约占用
  for(int i = 0; i < AGENT_LEASE_MAX; i++){
    if(agent_global.leases[i].used &&
       agent_global.leases[i].dev == ip->dev &&
       agent_global.leases[i].inum == ip->inum &&
       agent_global.leases[i].expiry_tick > now){
      release(&agent_global.lock);
      iunlockput(ip); end_op();
      return AGENT_TOOL_ERR_CONFLICT;
    }
  }

  for(int i = 0; i < AGENT_LEASE_MAX; i++){
    if(!agent_global.leases[i].used){
      slot = i;
      break;
    }
  }
  if(slot < 0){
    release(&agent_global.lock);
    iunlockput(ip); end_op();
    return AGENT_TOOL_ERR_NO_SPACE;
  }

  agent_global.leases[slot].used = 1;
  agent_global.leases[slot].dev = ip->dev;
  agent_global.leases[slot].inum = ip->inum;
  agent_global.leases[slot].owner_pid = p->pid;
  agent_global.leases[slot].owner_generation = p->identity_generation;
  agent_global.leases[slot].lease_id = ++agent_global.next_lease_id;
  agent_global.leases[slot].base_version = 0; // 简化: 用 inode 的 size 或 mtime
  agent_global.leases[slot].expiry_tick = now + AGENT_LEASE_EXPIRY_TICKS;

  *lease_id = agent_global.leases[slot].lease_id;
  *base_version = agent_global.leases[slot].base_version;

  uint audit_inum = ip->inum;
  release(&agent_global.lock);
  iunlockput(ip);
  end_op();

  agent_audit_record(p, (uint64)audit_inum, "lease_begin", 1, AGENT_TOOL_OK, "");
  return AGENT_TOOL_OK;
}

int
agent_proc_lease_commit(struct proc *p, uint64 lease_id, uint64 expected_version)
{
  int slot = -1;
  uint64 now = agent_now_safe();

  acquire(&agent_global.lock);
  for(int i = 0; i < AGENT_LEASE_MAX; i++){
    if(agent_global.leases[i].used &&
       agent_global.leases[i].lease_id == lease_id){
      slot = i;
      break;
    }
  }
  if(slot < 0 || agent_global.leases[slot].owner_pid != p->pid ||
     agent_global.leases[slot].owner_generation != p->identity_generation){
    release(&agent_global.lock);
    return AGENT_TOOL_ERR_PERMISSION;
  }
  if(agent_global.leases[slot].expiry_tick <= now){
    release(&agent_global.lock);
    return AGENT_TOOL_ERR_STALE;
  }
  // 简化版本检查: 当前实现不跟踪精确版本号
  // 未来可扩展为检查 agent_file_version
  memset(&agent_global.leases[slot], 0, sizeof(agent_global.leases[slot]));
  release(&agent_global.lock);

  agent_audit_record(p, lease_id, "lease_commit", 1, AGENT_TOOL_OK, "");
  return AGENT_TOOL_OK;
}

int
agent_proc_lease_abort(struct proc *p, uint64 lease_id)
{
  acquire(&agent_global.lock);
  for(int i = 0; i < AGENT_LEASE_MAX; i++){
    if(agent_global.leases[i].used &&
       agent_global.leases[i].lease_id == lease_id &&
       agent_global.leases[i].owner_pid == p->pid &&
       agent_global.leases[i].owner_generation == p->identity_generation){
      memset(&agent_global.leases[i], 0, sizeof(agent_global.leases[i]));
      release(&agent_global.lock);
      return AGENT_TOOL_OK;
    }
  }
  release(&agent_global.lock);
  return AGENT_TOOL_ERR_PERMISSION;
}

void
agent_lease_reap_expired(uint64 now)
{
  acquire(&agent_global.lock);
  for(int i = 0; i < AGENT_LEASE_MAX; i++){
    if(agent_global.leases[i].used &&
       agent_global.leases[i].expiry_tick <= now)
      memset(&agent_global.leases[i], 0, sizeof(agent_global.leases[i]));
  }
  release(&agent_global.lock);
}

void
agent_lease_reap_pid(int pid)
{
  acquire(&agent_global.lock);
  for(int i = 0; i < AGENT_LEASE_MAX; i++){
    if(agent_global.leases[i].used &&
       agent_global.leases[i].owner_pid == pid)
      memset(&agent_global.leases[i], 0, sizeof(agent_global.leases[i]));
  }
  release(&agent_global.lock);
}

// 辅助字符串构建函数 (导出给其他模块使用)
void
buf_putc_s(char **buf, int *left, char c)
{
  if(*left <= 1) return;
  **buf = c; (*buf)++; (*left)--; **buf = 0;
}

void
buf_puts_s(char **buf, int *left, const char *s)
{
  while(*s) buf_putc_s(buf, left, *s++);
}

void
buf_putu_s(char **buf, int *left, uint64 value)
{
  char tmp[24]; int n = 0;
  if(value == 0){ buf_putc_s(buf, left, '0'); return; }
  while(value > 0 && n < (int)sizeof(tmp)){ tmp[n++] = '0' + value % 10; value /= 10; }
  while(n > 0) buf_putc_s(buf, left, tmp[--n]);
}
