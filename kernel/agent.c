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

#define AGENT_FILE_META_MAX (96)
#define AGENT_FILE_PATH_MAX (64)
#define AGENT_FILE_INDEX_BUCKETS (67)
#define AGENT_FILE_RESULT_MAX (3)
#define AGENT_FILE_QUERY_COND_MAX (6)
#define AGENT_FILE_POSTING_MAX (AGENT_FILE_META_MAX * INODE_ATTR_MAX)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

extern struct proc proc[NPROC];

struct agent_file_meta {
  int used;
  uint inum;
  char path[AGENT_FILE_PATH_MAX];
  int attr_count;
  char summary[INODE_SUMMARY_MAX];
  struct inode_attr attrs[INODE_ATTR_MAX];
};

struct agent_file_posting {
  int used;
  int entry_idx;
  int next;
  char key[INODE_ATTR_KEY_MAX];
  char value[INODE_ATTR_VALUE_MAX];
};

struct agent_file_query_cond {
  char key[INODE_ATTR_KEY_MAX];
  char value[INODE_ATTR_VALUE_MAX];
};

static struct spinlock agent_file_lock;
static int agent_file_ready;
static int agent_file_index_ready;
static struct agent_file_meta file_meta[AGENT_FILE_META_MAX];
static struct agent_file_posting file_postings[AGENT_FILE_POSTING_MAX];
static int file_index[AGENT_FILE_INDEX_BUCKETS];

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

static void
agent_file_init(void)
{
  if(agent_file_ready)
    return;
  initlock(&agent_file_lock, "agent_file");
  for(int i = 0; i < AGENT_FILE_INDEX_BUCKETS; i++)
    file_index[i] = -1;
  agent_file_ready = 1;
}

static int
streq(const char *a, const char *b)
{
  return strncmp(a, b, MAX2(strlen(a), strlen(b)) + 1) == 0;
}

static int
str_contains(const char *s, const char *needle)
{
  int n = strlen(needle);

  if(n == 0)
    return 1;
  for(; *s; s++){
    if(strncmp(s, needle, n) == 0)
      return 1;
  }
  return 0;
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

static int
param_value(const char *params, const char *key, char *out, int outsz)
{
  int keylen = strlen(key);
  const char *p = params;

  while(*p){
    if(strncmp(p, key, keylen) == 0 && p[keylen] == '='){
      int n = 0;

      p += keylen + 1;
      while(*p && *p != ';' && n < outsz - 1)
        out[n++] = *p++;
      out[n] = 0;
      return 0;
    }
    while(*p && *p != ';')
      p++;
    if(*p == ';')
      p++;
  }
  if(outsz > 0)
    out[0] = 0;
  return -1;
}

static int
parse_uint_param(const char *params, const char *key, uint64 *value)
{
  char tmp[32];
  uint64 ret = 0;

  if(param_value(params, key, tmp, sizeof(tmp)) < 0 || tmp[0] == 0)
    return -1;
  for(int i = 0; tmp[i]; i++){
    if(tmp[i] < '0' || tmp[i] > '9')
      return -1;
    ret = ret * 10 + tmp[i] - '0';
  }
  *value = ret;
  return 0;
}

static uint
file_attr_hash(const char *key, const char *value)
{
  uint hash = 5381;

  while(*key)
    hash = ((hash << 5) + hash) + *key++;
  hash = ((hash << 5) + hash) + '=';
  while(*value)
    hash = ((hash << 5) + hash) + *value++;
  return hash % AGENT_FILE_INDEX_BUCKETS;
}

static int
file_attr_match(struct inode_attr *attrs, int attr_count,
                const char *key, const char *value)
{
  for(int i = 0; i < attr_count; i++){
    if(streq(attrs[i].key, key) && streq(attrs[i].value, value))
      return 1;
  }
  return 0;
}

static void
file_index_reset_locked(void)
{
  for(int i = 0; i < AGENT_FILE_INDEX_BUCKETS; i++)
    file_index[i] = -1;
  memset(file_postings, 0, sizeof(file_postings));
}

static int
file_add_posting_locked(int entry_idx, const char *key, const char *value)
{
  int slot = -1;
  uint h;

  for(int i = 0; i < AGENT_FILE_POSTING_MAX; i++){
    if(!file_postings[i].used){
      slot = i;
      break;
    }
  }
  if(slot < 0)
    return -1;
  h = file_attr_hash(key, value);
  file_postings[slot].used = 1;
  file_postings[slot].entry_idx = entry_idx;
  safestrcpy(file_postings[slot].key, key, sizeof(file_postings[slot].key));
  safestrcpy(file_postings[slot].value, value, sizeof(file_postings[slot].value));
  file_postings[slot].next = file_index[h];
  file_index[h] = slot;
  return 0;
}

static int
file_cache_find_by_inum_locked(uint inum)
{
  for(int i = 0; i < AGENT_FILE_META_MAX; i++){
    if(file_meta[i].used && file_meta[i].inum == inum)
      return i;
  }
  return -1;
}

static void
file_cache_fill_from_inode_locked(struct agent_file_meta *meta, struct inode *ip,
                                  const char *path)
{
  memset(meta, 0, sizeof(*meta));
  meta->used = 1;
  meta->inum = ip->inum;
  meta->attr_count = ip->attr_count;
  safestrcpy(meta->path, path, sizeof(meta->path));
  safestrcpy(meta->summary, ip->summary, sizeof(meta->summary));
  memmove(meta->attrs, ip->attrs, sizeof(meta->attrs));
}

static int
file_cache_upsert_locked(struct inode *ip, const char *path)
{
  int idx = file_cache_find_by_inum_locked(ip->inum);

  if(idx < 0){
    for(int i = 0; i < AGENT_FILE_META_MAX; i++){
      if(!file_meta[i].used){
        idx = i;
        break;
      }
    }
  }
  if(idx < 0)
    return -1;
  file_cache_fill_from_inode_locked(&file_meta[idx], ip, path);
  return idx;
}

static void
file_rebuild_postings_locked(void)
{
  file_index_reset_locked();
  for(int i = 0; i < AGENT_FILE_META_MAX; i++){
    if(!file_meta[i].used)
      continue;
    for(int j = 0; j < file_meta[i].attr_count; j++)
      file_add_posting_locked(i, file_meta[i].attrs[j].key,
                              file_meta[i].attrs[j].value);
  }
}

static int
file_join_path(const char *base, const char *name, char *out, int outsz)
{
  int pos = 0;

  if(base[0]){
    safestrcpy(out, base, outsz);
    pos = strlen(out);
    if(pos >= outsz - 1)
      return -1;
    out[pos++] = '/';
    out[pos] = 0;
  } else {
    out[0] = 0;
  }
  if(pos >= outsz - 1)
    return -1;
  safestrcpy(out + pos, name, outsz - pos);
  return 0;
}

static void
file_index_walk(struct inode *ip, const char *path)
{
  struct dirent de;
  uint off;

  if(ip->type == T_FILE){
    acquire(&agent_file_lock);
    file_cache_upsert_locked(ip, path);
    release(&agent_file_lock);
    return;
  }
  if(ip->type != T_DIR)
    return;

  for(off = 0; off + sizeof(de) <= ip->size; off += sizeof(de)){
    struct inode *child;
    char child_name[DIRSIZ + 1];
    char child_path[AGENT_FILE_PATH_MAX];

    if(readi(ip, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      break;
    if(de.inum == 0)
      continue;
    if(namecmp(de.name, ".") == 0 || namecmp(de.name, "..") == 0)
      continue;
    memset(child_name, 0, sizeof(child_name));
    memmove(child_name, de.name, DIRSIZ);
    if(file_join_path(path, child_name, child_path, sizeof(child_path)) < 0)
      continue;
    child = dirlookup(ip, child_name, 0);
    if(child == 0)
      continue;
    ilock(child);
    file_index_walk(child, child_path);
    iunlockput(child);
  }
}

static void
file_index_build(void)
{
  struct inode *root;

  agent_file_init();
  begin_op();
  root = namei("/");
  if(root == 0){
    end_op();
    return;
  }
  ilock(root);
  acquire(&agent_file_lock);
  memset(file_meta, 0, sizeof(file_meta));
  file_index_reset_locked();
  release(&agent_file_lock);
  file_index_walk(root, "");
  iunlockput(root);
  acquire(&agent_file_lock);
  file_rebuild_postings_locked();
  agent_file_index_ready = 1;
  release(&agent_file_lock);
  end_op();
}

static void
file_index_ensure(void)
{
  agent_file_init();
  acquire(&agent_file_lock);
  if(agent_file_index_ready){
    release(&agent_file_lock);
    return;
  }
  release(&agent_file_lock);
  file_index_build();
}

static int
file_query_parse(const char *params, struct agent_file_query_cond *conds,
                 int *cond_count, char *keyword, int keyword_sz, int *force_scan)
{
  const char *p = params;

  *cond_count = 0;
  *force_scan = 0;
  if(keyword_sz > 0)
    keyword[0] = 0;
  while(*p){
    char key[INODE_ATTR_KEY_MAX];
    char value[INODE_ATTR_VALUE_MAX];
    int kn = 0;
    int vn = 0;

    while(*p == ';')
      p++;
    if(*p == 0)
      break;
    while(*p && *p != '=' && *p != ';' && kn < sizeof(key) - 1)
      key[kn++] = *p++;
    key[kn] = 0;
    if(*p != '=')
      return -1;
    p++;
    while(*p && *p != ';' && vn < sizeof(value) - 1)
      value[vn++] = *p++;
    value[vn] = 0;
    if(streq(key, "keyword")){
      safestrcpy(keyword, value, keyword_sz);
    } else if(streq(key, "mode")){
      if(streq(value, "scan"))
        *force_scan = 1;
    } else if(*cond_count < AGENT_FILE_QUERY_COND_MAX){
      safestrcpy(conds[*cond_count].key, key, sizeof(conds[*cond_count].key));
      safestrcpy(conds[*cond_count].value, value,
                  sizeof(conds[*cond_count].value));
      (*cond_count)++;
    } else {
      return -1;
    }
    if(*p == ';')
      p++;
  }
  return 0;
}

static int
file_meta_matches(struct agent_file_meta *meta,
                  struct agent_file_query_cond *conds, int cond_count,
                  const char *keyword)
{
  for(int i = 0; i < cond_count; i++){
    if(!file_attr_match(meta->attrs, meta->attr_count,
                        conds[i].key, conds[i].value))
      return 0;
  }
  if(keyword[0] && !str_contains(meta->summary, keyword))
    return 0;
  return 1;
}

static void
append_file_result(char **ptr, int *left, struct agent_file_meta *meta)
{
  buf_puts(ptr, left, "{path=");
  buf_puts(ptr, left, meta->path);
  for(int i = 0; i < meta->attr_count; i++){
    buf_putc(ptr, left, ',');
    buf_puts(ptr, left, meta->attrs[i].key);
    buf_putc(ptr, left, '=');
    buf_puts(ptr, left, meta->attrs[i].value);
  }
  buf_puts(ptr, left, ",summary=");
  buf_puts(ptr, left, meta->summary);
  buf_putc(ptr, left, '}');
}

static void
inode_summary_refresh(struct inode *ip)
{
  int n;

  memset(ip->summary, 0, sizeof(ip->summary));
  n = readi(ip, 0, (uint64)ip->summary, 0, sizeof(ip->summary) - 1);
  if(n < 0)
    n = 0;
  ip->summary[n] = 0;
}

static uint64
agent_now(void)
{
  return ticks;
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
  memset(p->context_offsets, 0, sizeof(p->context_offsets));
  memset(p->context_lengths, 0, sizeof(p->context_lengths));
  p->watch_mask = 0;
  p->pending_events = 0;
  p->last_wakeup_reason = AGENT_EVENT_NONE;
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
  memmove(dst->context_offsets, src->context_offsets,
          sizeof(dst->context_offsets));
  memmove(dst->context_lengths, src->context_lengths,
          sizeof(dst->context_lengths));
  dst->watch_mask = src->watch_mask;
  dst->pending_events = src->pending_events;
  dst->last_wakeup_reason = src->last_wakeup_reason;
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
  info->sched_priority = p->agent_sched_priority;
  info->sched_quota = p->agent_sched_quota;
  info->sched_budget = p->agent_sched_budget;
  return 0;
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

int
agent_copy_tool_list(struct proc *p, uint64 dst, uint64 len)
{
  char tools[] =
    "get_system_status();query_process(type);send_message(target_pid,message);"
    "read_context();set_file_attr(path,key,value);get_file_attr(path,key);"
    "del_file_attr(path,key);query_file(type,owner,tags,keyword,mode)";
  uint64 n = MIN((uint64)strlen(tools), len);

  if(copyout(p->pagetable, dst, tools, n) < 0)
    return -1;
  return n;
}

static void
tool_resp_set(struct agent_tool_response *resp, int status, const char *result)
{
  memset(resp, 0, sizeof(*resp));
  resp->status = status;
  safestrcpy(resp->result, result, sizeof(resp->result));
  resp->result_len = strlen(resp->result);
}

static void
tool_get_system_status(struct agent_tool_response *resp)
{
  char buf[AGENT_TOOL_RESULT_MAX];
  char *ptr = buf;
  int left = sizeof(buf);
  int used = 0;
  int agents = 0;

  memset(buf, 0, sizeof(buf));
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state != UNUSED){
      used++;
      if(p->agent_type != AGENT_TYPE_NORMAL)
        agents++;
    }
  }
  buf_puts(&ptr, &left, "{status=ok,procs=");
  buf_putu(&ptr, &left, used);
  buf_puts(&ptr, &left, ",agents=");
  buf_putu(&ptr, &left, agents);
  buf_puts(&ptr, &left, ",ticks=");
  buf_putu(&ptr, &left, ticks);
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf);
}

static void
tool_query_process(struct agent_tool_request *req,
                   struct agent_tool_response *resp)
{
  char type[24];
  char buf[AGENT_TOOL_RESULT_MAX];
  char *ptr = buf;
  int left = sizeof(buf);
  int count = 0;
  int only_agent = 0;

  memset(buf, 0, sizeof(buf));
  if(param_value(req->params, "type", type, sizeof(type)) == 0 &&
     streq(type, "agent"))
    only_agent = 1;
  buf_puts(&ptr, &left, "{status=ok,processes=[");
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(only_agent && p->agent_type == AGENT_TYPE_NORMAL)
      continue;
    if(count > 0)
      buf_putc(&ptr, &left, ',');
    buf_puts(&ptr, &left, "{pid=");
    buf_putu(&ptr, &left, p->pid);
    buf_puts(&ptr, &left, ",name=");
    buf_puts(&ptr, &left, p->name);
    buf_puts(&ptr, &left, ",type=");
    buf_putu(&ptr, &left, p->agent_type);
    buf_putc(&ptr, &left, '}');
    count++;
  }
  buf_puts(&ptr, &left, "],count=");
  buf_putu(&ptr, &left, count);
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf);
}

static void
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

static void
tool_send_message(struct agent_tool_request *req,
                  struct agent_tool_response *resp)
{
  uint64 pid;
  char message[AGENT_MESSAGE_MAX];
  struct proc *target = 0;

  if(parse_uint_param(req->params, "target_pid", &pid) < 0 ||
     param_value(req->params, "message", message, sizeof(message)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params");
    return;
  }
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->pid == pid && p->state != UNUSED){
      target = p;
      break;
    }
  }
  if(target == 0 || target->agent_type == AGENT_TYPE_NORMAL){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "target not found");
    return;
  }
  acquire(&target->lock);
  safestrcpy(target->agent_message, message, sizeof(target->agent_message));
  if(target->watch_mask & AGENT_WATCH_MESSAGE){
    agent_signal_event_locked(target, AGENT_EVENT_MESSAGE, agent_now_safe());
  }
  release(&target->lock);
  tool_resp_set(resp, AGENT_TOOL_OK, "message delivered");
}

static void
tool_read_context(struct proc *p, struct agent_tool_response *resp)
{
  char tmp[AGENT_TOOL_RESULT_MAX];
  uint64 n = MIN((uint64)(sizeof(tmp) - 1), p->context_path_len);

  if(n == 0){
    tool_resp_set(resp, AGENT_TOOL_OK, "");
    return;
  }
  if(copyin(p->pagetable, tmp, agent_path_base(p) + p->context_path_len - n,
            n) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "context unavailable");
    return;
  }
  tmp[n] = 0;
  tool_resp_set(resp, AGENT_TOOL_OK, tmp);
}

static void
tool_set_file_attr(struct agent_tool_request *req,
                   struct agent_tool_response *resp)
{
  char path[AGENT_FILE_PATH_MAX], key[INODE_ATTR_KEY_MAX];
  char value[INODE_ATTR_VALUE_MAX];
  struct inode *ip;
  int set = 0;

  if(param_value(req->params, "path", path, sizeof(path)) < 0 ||
     param_value(req->params, "key", key, sizeof(key)) < 0 ||
     param_value(req->params, "value", value, sizeof(value)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params");
    return;
  }
  begin_op();
  ip = namei(path);
  if(ip == 0){
    end_op();
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file not found");
    return;
  }
  ilock(ip);
  if(ip->type != T_FILE){
    iunlockput(ip);
    end_op();
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "not a file");
    return;
  }
  for(int i = 0; i < ip->attr_count; i++){
    if(streq(ip->attrs[i].key, key)){
      safestrcpy(ip->attrs[i].value, value, sizeof(ip->attrs[i].value));
      set = 1;
      break;
    }
  }
  if(!set){
    if(ip->attr_count >= INODE_ATTR_MAX){
      iunlockput(ip);
      end_op();
      tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "too many attrs");
      return;
    }
    safestrcpy(ip->attrs[ip->attr_count].key, key,
               sizeof(ip->attrs[ip->attr_count].key));
    safestrcpy(ip->attrs[ip->attr_count].value, value,
               sizeof(ip->attrs[ip->attr_count].value));
    ip->attr_count++;
  }
  inode_summary_refresh(ip);
  iupdate(ip);
  iunlockput(ip);
  end_op();
  file_index_build();
  tool_resp_set(resp, AGENT_TOOL_OK, "attr set");
}

static void
tool_get_file_attr(struct agent_tool_request *req,
                   struct agent_tool_response *resp)
{
  char path[AGENT_FILE_PATH_MAX], key[INODE_ATTR_KEY_MAX];
  char buf[AGENT_TOOL_RESULT_MAX];
  struct inode *ip;

  if(param_value(req->params, "path", path, sizeof(path)) < 0 ||
     param_value(req->params, "key", key, sizeof(key)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params");
    return;
  }
  begin_op();
  ip = namei(path);
  if(ip == 0){
    end_op();
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file not found");
    return;
  }
  ilock(ip);
  for(int i = 0; i < ip->attr_count; i++){
    if(streq(ip->attrs[i].key, key)){
      char *ptr = buf;
      int left = sizeof(buf);

      memset(buf, 0, sizeof(buf));
      buf_puts(&ptr, &left, "{status=ok,path=");
      buf_puts(&ptr, &left, path);
      buf_putc(&ptr, &left, ',');
      buf_puts(&ptr, &left, key);
      buf_putc(&ptr, &left, '=');
      buf_puts(&ptr, &left, ip->attrs[i].value);
      buf_putc(&ptr, &left, '}');
      iunlockput(ip);
      end_op();
      tool_resp_set(resp, AGENT_TOOL_OK, buf);
      return;
    }
  }
  iunlockput(ip);
  end_op();
  tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "attr not found");
}

static void
tool_del_file_attr(struct agent_tool_request *req,
                   struct agent_tool_response *resp)
{
  char path[AGENT_FILE_PATH_MAX], key[INODE_ATTR_KEY_MAX];
  struct inode *ip;

  if(param_value(req->params, "path", path, sizeof(path)) < 0 ||
     param_value(req->params, "key", key, sizeof(key)) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params");
    return;
  }
  begin_op();
  ip = namei(path);
  if(ip == 0){
    end_op();
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file not found");
    return;
  }
  ilock(ip);
  for(int i = 0; i < ip->attr_count; i++){
    if(streq(ip->attrs[i].key, key)){
      for(int j = i + 1; j < ip->attr_count; j++)
        ip->attrs[j - 1] = ip->attrs[j];
      memset(&ip->attrs[ip->attr_count - 1], 0, sizeof(ip->attrs[0]));
      ip->attr_count--;
      iupdate(ip);
      iunlockput(ip);
      end_op();
      file_index_build();
      tool_resp_set(resp, AGENT_TOOL_OK, "attr deleted");
      return;
    }
  }
  iunlockput(ip);
  end_op();
  tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "attr not found");
}

static void
tool_query_file(struct agent_tool_request *req, struct agent_tool_response *resp)
{
  struct agent_file_query_cond conds[AGENT_FILE_QUERY_COND_MAX];
  char keyword[INODE_ATTR_VALUE_MAX];
  char buf[AGENT_TOOL_RESULT_MAX];
  char *ptr = buf;
  int left = sizeof(buf);
  int cond_count;
  int force_scan;
  int count = 0;
  int index_scanned = 0;
  int full_scanned = 0;
  int used_index = 0;
  uint64 ticks_begin;
  uint64 ticks_end;

  file_index_ensure();
  if(file_query_parse(req->params, conds, &cond_count, keyword,
                      sizeof(keyword), &force_scan) < 0){
    tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params");
    return;
  }
  memset(buf, 0, sizeof(buf));
  buf_puts(&ptr, &left, "{status=ok,files=[");
  acquire(&agent_file_lock);
  ticks_begin = agent_now_safe();
  for(int i = 0; i < AGENT_FILE_META_MAX; i++)
    if(file_meta[i].used)
      full_scanned++;
  if(cond_count > 0 && !force_scan){
    uint h = file_attr_hash(conds[0].key, conds[0].value);
    for(int post = file_index[h]; post >= 0; post = file_postings[post].next){
      struct agent_file_meta *meta;

      index_scanned++;
      if(!streq(file_postings[post].key, conds[0].key) ||
         !streq(file_postings[post].value, conds[0].value))
        continue;
      if(file_postings[post].entry_idx < 0 ||
         file_postings[post].entry_idx >= AGENT_FILE_META_MAX)
        continue;
      meta = &file_meta[file_postings[post].entry_idx];
      if(!meta->used)
        continue;
      if(file_meta_matches(meta, conds, cond_count, keyword)){
        if(count < AGENT_FILE_RESULT_MAX){
          if(count > 0)
            buf_putc(&ptr, &left, ',');
          append_file_result(&ptr, &left, meta);
        }
        count++;
      }
    }
    used_index = 1;
  } else {
    for(int i = 0; i < AGENT_FILE_META_MAX; i++){
      struct agent_file_meta *meta = &file_meta[i];

      if(!meta->used)
        continue;
      index_scanned++;
      if(file_meta_matches(meta, conds, cond_count, keyword)){
        if(count < AGENT_FILE_RESULT_MAX){
          if(count > 0)
            buf_putc(&ptr, &left, ',');
          append_file_result(&ptr, &left, meta);
        }
        count++;
      }
    }
  }
  ticks_end = agent_now_safe();
  release(&agent_file_lock);
  buf_puts(&ptr, &left, "],count=");
  buf_putu(&ptr, &left, count);
  buf_puts(&ptr, &left, ",used_index=");
  buf_putu(&ptr, &left, used_index);
  buf_puts(&ptr, &left, ",index_scanned=");
  buf_putu(&ptr, &left, index_scanned);
  buf_puts(&ptr, &left, ",full_scanned=");
  buf_putu(&ptr, &left, full_scanned);
  buf_puts(&ptr, &left, ",ticks_cost=");
  buf_putu(&ptr, &left, ticks_end - ticks_begin);
  buf_putc(&ptr, &left, '}');
  tool_resp_set(resp, AGENT_TOOL_OK, buf);
}

int
agent_tool_call(struct proc *p, struct agent_tool_request *req,
                struct agent_tool_response *resp)
{
  struct agent_context_node node;

  if(p->agent_type == AGENT_TYPE_NORMAL){
    tool_resp_set(resp, AGENT_TOOL_ERR_NOT_AGENT, "process is not agent");
    return resp->status;
  }
  p->loop_state = AGENT_LOOP_RUNNING;
  if(streq(req->tool, "query_process")){
    tool_query_process(req, resp);
  } else if(streq(req->tool, "get_system_status")){
    tool_get_system_status(resp);
  } else if(streq(req->tool, "send_message")){
    tool_send_message(req, resp);
  } else if(streq(req->tool, "read_context")){
    tool_read_context(p, resp);
  } else if(streq(req->tool, "set_file_attr")){
    tool_set_file_attr(req, resp);
  } else if(streq(req->tool, "get_file_attr")){
    tool_get_file_attr(req, resp);
  } else if(streq(req->tool, "del_file_attr")){
    tool_del_file_attr(req, resp);
  } else if(streq(req->tool, "query_file")){
    tool_query_file(req, resp);
  } else {
    tool_resp_set(resp, AGENT_TOOL_ERR_TOOL_NOT_FOUND, "tool not found");
  }

  memset(&node, 0, sizeof(node));
  node.timestamp_ms = agent_now();
  safestrcpy(node.request, req->tool, sizeof(node.request));
  if(req->params[0]){
    int n = strlen(node.request);
    if(n < sizeof(node.request) - 2){
      node.request[n] = '(';
      safestrcpy(node.request + n + 1, req->params,
                 sizeof(node.request) - n - 1);
      n = strlen(node.request);
      if(n < sizeof(node.request) - 1){
        node.request[n] = ')';
        node.request[n + 1] = 0;
      }
    }
  }
  safestrcpy(node.result, resp->result, sizeof(node.result));
  agent_context_push_node(p, &node);
  p->loop_state = AGENT_LOOP_READY;
  return resp->status;
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
  p->heartbeat_deadline = agent_now_safe() + interval;
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
