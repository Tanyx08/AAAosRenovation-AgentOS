// Agent Context 区与 Context Path 管理。
//
// 决赛改进:
// - #6: 内核可信 Context 摘要 (在 agent.c 中实现, push 时调用)
// - #7: 固定 Context ABI: magic/version/header_size/node_size/generation/sequence
// - #7: guard page (在 agent.c 的 agent_mark_current 中分配)
// - #8: Context 复用跟踪
// - #23: 数据驱动的节点布局 (保持变长但增加元数据)

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

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static uint64 agent_now(void);
static void buf_putc(char **buf, int *left, char c);
static void buf_puts(char **buf, int *left, const char *s);
static void buf_putu(char **buf, int *left, uint64 value);

static uint64 agent_now(void) { return ticks; }

static void buf_putc(char **buf, int *left, char c)
{ if(*left <= 1) return; **buf = c; (*buf)++; (*left)--; **buf = 0; }

static void buf_puts(char **buf, int *left, const char *s)
{ while(*s) buf_putc(buf, left, *s++); }

static void buf_putu(char **buf, int *left, uint64 value)
{
  char tmp[24]; int n = 0;
  if(value == 0){ buf_putc(buf, left, '0'); return; }
  while(value > 0 && n < (int)sizeof(tmp)){ tmp[n++] = '0' + value % 10; value /= 10; }
  while(n > 0) buf_putc(buf, left, tmp[--n]);
}

static uint64
agent_path_capacity(struct proc *p)
{
  uint64 cap = p->context_region_size - sizeof(struct agent_context_header);
  if(p->resource_quota == 0 || p->resource_quota > cap)
    return cap;
  return p->resource_quota;
}

static uint64
agent_path_base(struct proc *p)
{
  return p->context_region_start + sizeof(struct agent_context_header);
}

static int
agent_context_write_header(struct proc *p, struct agent_context_header *hdr)
{
  if(p->context_region_start == 0)
    return -1;
  return copyout(p->pagetable, p->context_region_start, (char*)hdr,
                 sizeof(*hdr));
}

// 修改点 #7: 固定 ABI 的 header 构建
static void
agent_context_build_header(struct proc *p, struct agent_context_header *hdr)
{
  memset(hdr, 0, sizeof(*hdr));
  hdr->magic = AGENT_CONTEXT_HEADER_MAGIC;
  hdr->version = AGENT_CONTEXT_HEADER_VERSION;
  hdr->header_size = sizeof(struct agent_context_header);
  hdr->node_size = sizeof(struct agent_context_node);
  hdr->region_size = p->context_region_size;
  hdr->path_offset = sizeof(struct agent_context_header);
  hdr->path_length = p->context_path_len;
  hdr->node_count = p->context_node_count;
  hdr->dropped_nodes = p->context_dropped_nodes;
  hdr->generation = p->context_generation;
  hdr->first_sequence = p->context_first_sequence;
  hdr->next_sequence = p->context_next_sequence;
  hdr->last_timestamp = agent_now();
}

// 修改点 #7: 更新 header 的 last_result 信息
static void
agent_context_fill_last(struct proc *p, struct agent_context_header *hdr)
{
  char tmp[128];
  uint64 n = MIN(p->context_path_len, (uint64)(sizeof(tmp) - 1));
  if(n > 0 &&
     copyin(p->pagetable, tmp, agent_path_base(p) + p->context_path_len - n, n) == 0){
    tmp[n] = 0;
    safestrcpy(hdr->last_result, tmp, sizeof(hdr->last_result));
    hdr->last_result_len = strlen(hdr->last_result);
  }
}

static void
agent_evict_oldest(struct proc *p)
{
  uint64 base = agent_path_base(p);
  uint64 first_len, remain;
  char tmp[128];
  uint64 copied = 0;

  if(p->context_node_count == 0){
    p->context_path_len = 0;
    return;
  }
  first_len = p->context_lengths[0];
  if(first_len > p->context_path_len)
    first_len = p->context_path_len;
  remain = p->context_path_len - first_len;
  while(copied < remain){
    uint64 chunk = MIN((uint64)sizeof(tmp), remain - copied);
    if(copyin(p->pagetable, tmp, base + first_len + copied, chunk) < 0 ||
       copyout(p->pagetable, base + copied, tmp, chunk) < 0){
      p->context_path_len = 0;
      p->context_node_count = 0;
      return;
    }
    copied += chunk;
  }
  p->context_path_len -= first_len;
  for(uint64 i = 1; i < p->context_node_count; i++){
    p->context_offsets[i - 1] = p->context_offsets[i] - first_len;
    p->context_lengths[i - 1] = p->context_lengths[i];
  }
  if(p->context_node_count > 0)
    p->context_node_count--;
  p->context_dropped_nodes++;
  p->context_first_sequence++; // 首个淘汰后递增
}

// 修改点 #7: 增强的 header 同步（带 generation）
int
agent_sync_header(struct proc *p)
{
  struct agent_context_header hdr;

  // 修改点 #7: generation 递增确保用户态读取一致性
  p->context_generation++;

  agent_context_build_header(p, &hdr);
  agent_context_fill_last(p, &hdr);
  return agent_context_write_header(p, &hdr);
}

void
agent_context_clear(struct proc *p)
{
  char zero[128];
  uint64 cleared = 0;

  p->context_path_len = 0;
  p->context_node_count = 0;
  p->context_dropped_nodes = 0;
  p->context_generation++;
  p->context_first_sequence = 0;
  p->context_next_sequence = 0;
  memset(p->context_offsets, 0, sizeof(p->context_offsets));
  memset(p->context_lengths, 0, sizeof(p->context_lengths));
  if(p->context_region_start == 0 || p->context_region_size == 0)
    return;
  memset(zero, 0, sizeof(zero));
  while(cleared < p->context_region_size){
    uint64 n = MIN((uint64)sizeof(zero), p->context_region_size - cleared);
    if(copyout(p->pagetable, p->context_region_start + cleared, zero, n) < 0)
      break;
    cleared += n;
  }
  // 清空内核可信摘要
  memset(p->context_digests, 0, sizeof(p->context_digests));
  p->context_digest_head = 0;
  p->context_digest_count = 0;

  agent_sync_header(p);
}

// 修改点 #7: 增强的 push (带 sequence/request_id/span_id/cause_sequence)
// 修改点 #8: Context 复用 hash 记录
int
agent_context_push_node(struct proc *p, struct agent_context_node *node)
{
  char record[AGENT_CONTEXT_REQ_MAX + AGENT_CONTEXT_RES_MAX + 128];
  char *ptr = record;
  int left = sizeof(record);
  uint64 rec_len;
  uint64 base;

  if(p->context_region_start == 0)
    return -1;

  memset(record, 0, sizeof(record));
  // 修改点 #7/#19: 扩展的记录格式
  buf_puts(&ptr, &left, "{ts=");
  buf_putu(&ptr, &left, node->timestamp_ms);
  buf_puts(&ptr, &left, ",seq=");
  buf_putu(&ptr, &left, node->sequence);
  buf_puts(&ptr, &left, ",req_id=");
  buf_putu(&ptr, &left, node->request_id);
  buf_puts(&ptr, &left, ",span=");
  buf_putu(&ptr, &left, node->span_id);
  buf_puts(&ptr, &left, ",cause=");
  buf_putu(&ptr, &left, node->cause_sequence);
  buf_puts(&ptr, &left, ",status=");
  buf_putu(&ptr, &left, node->status);
  buf_puts(&ptr, &left, ",req=");
  buf_puts(&ptr, &left, node->request);
  buf_puts(&ptr, &left, ",res=");
  buf_puts(&ptr, &left, node->result);
  buf_puts(&ptr, &left, "}\n");
  rec_len = strlen(record);
  base = agent_path_base(p);

  while(p->context_node_count >= AGENT_CONTEXT_MAX_NODES ||
        p->context_path_len + rec_len > agent_path_capacity(p))
    agent_evict_oldest(p);
  if(rec_len > agent_path_capacity(p))
    return -1;
  if(copyout(p->pagetable, base + p->context_path_len, record, rec_len) < 0)
    return -1;

  // 记录第一个 sequence
  if(p->context_node_count == 0)
    p->context_first_sequence = node->sequence;

  p->context_offsets[p->context_node_count] = p->context_path_len;
  p->context_lengths[p->context_node_count] = rec_len;
  p->context_path_len += rec_len;
  p->context_node_count++;

  // 修改点 #8: 记录查询 hash 用于复用
  p->last_context_query = node->request_id; // 简化: 用 request_id 代表查询

  return agent_sync_header(p);
}

int
agent_context_query(struct proc *p, uint64 dst, uint64 len)
{
  char tmp[128];
  uint64 copied = 0;
  uint64 n = MIN(len, p->context_path_len);

  // 修改点 #8: 标记 context_reuse_hit
  p->context_reuse_hit = 1;

  while(copied < n){
    uint64 chunk = MIN((uint64)sizeof(tmp), n - copied);
    if(copyin(p->pagetable, tmp, agent_path_base(p) + copied, chunk) < 0 ||
       copyout(p->pagetable, dst + copied, tmp, chunk) < 0)
      return -1;
    copied += chunk;
  }
  return n;
}

// 修改点 #7: rollback 后 generation 递增
int
agent_context_rollback(struct proc *p, uint64 keep_nodes)
{
  if(keep_nodes > p->context_node_count)
    return -1;
  if(keep_nodes == 0){
    agent_context_clear(p);
    return 0;
  }
  p->context_path_len = p->context_offsets[keep_nodes - 1] +
                        p->context_lengths[keep_nodes - 1];
  p->context_node_count = keep_nodes;
  p->context_next_sequence = p->context_first_sequence + keep_nodes;
  p->loop_state = AGENT_LOOP_ROLLED_BACK;
  p->context_generation++; // 修改点 #7: rollback 递增 generation
  return agent_sync_header(p);
}
