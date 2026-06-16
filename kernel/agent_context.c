// Agent Context 区与 Context Path 管理。
//
// 这个文件集中实现任务一与任务三最直接的内核能力：
// 用户态 Agent Context 区的布局维护，以及 Context Path 的追加、
// 查询、回滚、清空和淘汰逻辑。

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

static uint64
agent_now(void)
{
  return ticks;
}

static void
buf_putc(char **buf, int *left, char c)
{
  if(*left <= 1)
    return;
  **buf = c;
  (*buf)++;
  (*left)--;
  **buf = 0;
}

static void
buf_puts(char **buf, int *left, const char *s)
{
  while(*s)
    buf_putc(buf, left, *s++);
}

static void
buf_putu(char **buf, int *left, uint64 value)
{
  char tmp[24];
  int n = 0;

  if(value == 0){
    buf_putc(buf, left, '0');
    return;
  }
  while(value > 0 && n < sizeof(tmp)){
    tmp[n++] = '0' + value % 10;
    value /= 10;
  }
  while(n > 0)
    buf_putc(buf, left, tmp[--n]);
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

static void
agent_evict_oldest(struct proc *p)
{
  uint64 base = agent_path_base(p);
  uint64 first_len;
  uint64 remain;
  char tmp[AGENT_CONTEXT_REGION_SIZE];

  if(p->context_node_count == 0){
    p->context_path_len = 0;
    return;
  }
  first_len = p->context_lengths[0];
  if(first_len > p->context_path_len)
    first_len = p->context_path_len;
  remain = p->context_path_len - first_len;
  if(remain > sizeof(tmp))
    remain = sizeof(tmp);
  if(remain > 0){
    if(copyin(p->pagetable, tmp, base + first_len, remain) < 0 ||
       copyout(p->pagetable, base, tmp, remain) < 0){
      p->context_path_len = 0;
      p->context_node_count = 0;
      return;
    }
  }
  p->context_path_len -= first_len;
  for(uint64 i = 1; i < p->context_node_count; i++){
    p->context_offsets[i - 1] = p->context_offsets[i] - first_len;
    p->context_lengths[i - 1] = p->context_lengths[i];
  }
  if(p->context_node_count > 0)
    p->context_node_count--;
  p->context_dropped_nodes++;
}

int
agent_sync_header(struct proc *p)
{
  struct agent_context_header hdr;
  char tmp[AGENT_TOOL_RESULT_MAX];
  uint64 n = MIN(p->context_path_len, (uint64)(sizeof(tmp) - 1));

  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = AGENT_CONTEXT_HEADER_MAGIC;
  hdr.version = AGENT_CONTEXT_HEADER_VERSION;
  hdr.region_size = p->context_region_size;
  hdr.path_offset = sizeof(struct agent_context_header);
  hdr.path_length = p->context_path_len;
  hdr.node_count = p->context_node_count;
  hdr.dropped_nodes = p->context_dropped_nodes;
  hdr.last_timestamp = agent_now();
  if(n > 0 &&
     copyin(p->pagetable, tmp, agent_path_base(p) + p->context_path_len - n,
            n) == 0){
    tmp[n] = 0;
    safestrcpy(hdr.last_result, tmp, sizeof(hdr.last_result));
    hdr.last_result_len = strlen(hdr.last_result);
  }
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
  agent_sync_header(p);
}

int
agent_context_push_node(struct proc *p, struct agent_context_node *node)
{
  char record[AGENT_CONTEXT_REQ_MAX + AGENT_CONTEXT_RES_MAX + 48];
  char *ptr = record;
  int left = sizeof(record);
  uint64 rec_len;
  uint64 base;

  if(p->context_region_start == 0)
    return -1;
  memset(record, 0, sizeof(record));
  buf_puts(&ptr, &left, "{ts=");
  buf_putu(&ptr, &left, node->timestamp_ms);
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
  p->context_offsets[p->context_node_count] = p->context_path_len;
  p->context_lengths[p->context_node_count] = rec_len;
  p->context_path_len += rec_len;
  p->context_node_count++;
  return agent_sync_header(p);
}

int
agent_context_query(struct proc *p, uint64 dst, uint64 len)
{
  char tmp[128];
  uint64 copied = 0;
  uint64 n = MIN(len, p->context_path_len);

  while(copied < n){
    uint64 chunk = MIN((uint64)sizeof(tmp), n - copied);
    if(copyin(p->pagetable, tmp, agent_path_base(p) + copied, chunk) < 0 ||
       copyout(p->pagetable, dst + copied, tmp, chunk) < 0)
      return -1;
    copied += chunk;
  }
  return n;
}

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
  p->loop_state = AGENT_LOOP_ROLLED_BACK;
  return agent_sync_header(p);
}
