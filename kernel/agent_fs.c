// AgentFS 文件系统扩展与查询优化。
//
// 决赛改进:
// - #8: 跨 Agent shared cache 复用闭环
// - #16: 强化 FILEMOD 精确性 (dev/inum 精确匹配已在 agent_loop.c 中实现)

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
#define AGENT_SHARED_QUERY_CACHE_MAX (8)
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

struct agent_file_meta {
  int used;
  uint inum;
  uint dev;                  // 修改点 #16: 记录 dev
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

struct shared_query_cache {
  int used;
  char query[AGENT_TOOL_PARAM_MAX];
  char result[AGENT_TOOL_RESULT_MAX];
  int owner_pid;
  int owner_group;
  int refcnt;
  uint64 version;
};

static struct spinlock agent_file_lock;
static int agent_file_ready;
static int agent_file_index_ready;
static struct agent_file_meta file_meta[AGENT_FILE_META_MAX];
static struct agent_file_posting file_postings[AGENT_FILE_POSTING_MAX];
static int file_index[AGENT_FILE_INDEX_BUCKETS];
static struct spinlock agent_fs_runtime_lock;
static int agent_fs_runtime_ready;
static uint64 agent_file_version = 1;
static struct shared_query_cache shared_query_cache[AGENT_SHARED_QUERY_CACHE_MAX];

// 修改点 #8: 缓存命中统计
static uint64 shared_cache_hits = 0;
static uint64 shared_cache_misses = 0;

static void agent_fs_runtime_init(void);
static void agent_file_init(void);
static int streq(const char *a, const char *b);
static int str_contains(const char *s, const char *needle);
static int param_value(const char *params, const char *key, char *out, int outsz);
static void tool_resp_set(struct agent_tool_response *resp, int status, const char *result);
static uint64 agent_now_safe(void);
static uint file_attr_hash(const char *key, const char *value);
static int file_attr_match(struct inode_attr *attrs, int attr_count, const char *key, const char *value);
static void file_index_reset_locked(void);
static int file_add_posting_locked(int entry_idx, const char *key, const char *value);
static int file_cache_find_by_inum_locked(uint inum);
static void file_cache_fill_from_inode_locked(struct agent_file_meta *meta, struct inode *ip, const char *path);
static int file_cache_upsert_locked(struct inode *ip, const char *path);
static void file_rebuild_postings_locked(void);
static int file_join_path(const char *base, const char *name, char *out, int outsz);
static void file_index_walk(struct inode *ip, const char *path);
static void file_index_build(void);
static void file_index_ensure(void);
static int file_query_parse(const char *params, struct agent_file_query_cond *conds, int *cond_count, char *keyword, int keyword_sz, int *force_scan);
static int file_meta_matches(struct agent_file_meta *meta, struct agent_file_query_cond *conds, int cond_count, const char *keyword);
static void append_file_result(char **ptr, int *left, struct agent_file_meta *meta);
static int query_allows_public_share(const char *params);
static int cache_access_allowed(struct proc *p, struct shared_query_cache *entry);
static void query_result_with_cache(char *dst, int dstsz, const char *base, int hit, int owner_pid, int refcnt, uint64 version, int fs_scanned);
static int shared_query_cache_lookup(struct proc *p, const char *query, struct agent_tool_response *resp);
static void shared_query_cache_store(struct proc *p, const char *query, const char *result);
static void agent_file_version_bump(void);
static void inode_summary_refresh(struct inode *ip);

// 辅助函数声明
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

static void agent_fs_runtime_init(void)
{
  if(agent_fs_runtime_ready) return;
  initlock(&agent_fs_runtime_lock, "agent_fs_runtime");
  agent_fs_runtime_ready = 1;
}

static void agent_file_init(void)
{
  if(agent_file_ready) return;
  initlock(&agent_file_lock, "agent_file");
  for(int i = 0; i < AGENT_FILE_INDEX_BUCKETS; i++) file_index[i] = -1;
  agent_file_ready = 1;
}

static int streq(const char *a, const char *b)
{ return strncmp(a, b, MAX2(strlen(a), strlen(b)) + 1) == 0; }

static int str_contains(const char *s, const char *needle)
{
  int n = strlen(needle);
  if(n == 0) return 1;
  for(; *s; s++){ if(strncmp(s, needle, n) == 0) return 1; }
  return 0;
}

static int param_value(const char *params, const char *key, char *out, int outsz)
{
  int keylen = strlen(key); const char *p = params;
  while(*p){
    if(strncmp(p, key, keylen) == 0 && p[keylen] == '='){
      int n = 0; p += keylen + 1;
      while(*p && *p != ';' && n < outsz - 1) out[n++] = *p++;
      out[n] = 0; return 0;
    }
    while(*p && *p != ';') p++;
    if(*p == ';') p++;
  }
  if(outsz > 0) out[0] = 0;
  return -1;
}

static void tool_resp_set(struct agent_tool_response *resp, int status, const char *result)
{ memset(resp, 0, sizeof(*resp)); resp->status = status; safestrcpy(resp->result, result, sizeof(resp->result)); resp->result_len = strlen(resp->result); }

static uint64 agent_now_safe(void)
{ uint64 now; acquire(&tickslock); now = ticks; release(&tickslock); return now; }

static uint file_attr_hash(const char *key, const char *value)
{
  uint hash = 5381;
  while(*key) hash = ((hash << 5) + hash) + *key++;
  hash = ((hash << 5) + hash) + '=';
  while(*value) hash = ((hash << 5) + hash) + *value++;
  return hash % AGENT_FILE_INDEX_BUCKETS;
}

static int file_attr_match(struct inode_attr *attrs, int attr_count, const char *key, const char *value)
{
  for(int i = 0; i < attr_count; i++){
    if(streq(attrs[i].key, key) && streq(attrs[i].value, value)) return 1;
  }
  return 0;
}

static void file_index_reset_locked(void)
{
  for(int i = 0; i < AGENT_FILE_INDEX_BUCKETS; i++) file_index[i] = -1;
  memset(file_postings, 0, sizeof(file_postings));
}

static int file_add_posting_locked(int entry_idx, const char *key, const char *value)
{
  int slot = -1; uint h;
  for(int i = 0; i < AGENT_FILE_POSTING_MAX; i++){ if(!file_postings[i].used){ slot = i; break; } }
  if(slot < 0) return -1;
  h = file_attr_hash(key, value);
  file_postings[slot].used = 1; file_postings[slot].entry_idx = entry_idx;
  safestrcpy(file_postings[slot].key, key, sizeof(file_postings[slot].key));
  safestrcpy(file_postings[slot].value, value, sizeof(file_postings[slot].value));
  file_postings[slot].next = file_index[h]; file_index[h] = slot;
  return 0;
}

static int file_cache_find_by_inum_locked(uint inum)
{
  for(int i = 0; i < AGENT_FILE_META_MAX; i++){ if(file_meta[i].used && file_meta[i].inum == inum) return i; }
  return -1;
}

static void file_cache_fill_from_inode_locked(struct agent_file_meta *meta, struct inode *ip, const char *path)
{
  memset(meta, 0, sizeof(*meta)); meta->used = 1;
  meta->inum = ip->inum; meta->dev = ip->dev;
  meta->attr_count = ip->attr_count;
  safestrcpy(meta->path, path, sizeof(meta->path));
  safestrcpy(meta->summary, ip->summary, sizeof(meta->summary));
  memmove(meta->attrs, ip->attrs, sizeof(meta->attrs));
}

static int file_cache_upsert_locked(struct inode *ip, const char *path)
{
  int idx = file_cache_find_by_inum_locked(ip->inum);
  if(idx < 0){ for(int i = 0; i < AGENT_FILE_META_MAX; i++){ if(!file_meta[i].used){ idx = i; break; } } }
  if(idx < 0) return -1;
  file_cache_fill_from_inode_locked(&file_meta[idx], ip, path);
  return idx;
}

static void file_rebuild_postings_locked(void)
{
  file_index_reset_locked();
  for(int i = 0; i < AGENT_FILE_META_MAX; i++){
    if(!file_meta[i].used) continue;
    for(int j = 0; j < file_meta[i].attr_count; j++)
      file_add_posting_locked(i, file_meta[i].attrs[j].key, file_meta[i].attrs[j].value);
  }
}

static int file_join_path(const char *base, const char *name, char *out, int outsz)
{
  int pos = 0;
  if(base[0]){ safestrcpy(out, base, outsz); pos = strlen(out); if(pos >= outsz - 1) return -1; out[pos++] = '/'; out[pos] = 0; }
  else { out[0] = 0; }
  if(pos >= outsz - 1) return -1;
  safestrcpy(out + pos, name, outsz - pos); return 0;
}

static void file_index_walk(struct inode *ip, const char *path)
{
  struct dirent de; uint off;
  if(ip->type == T_FILE){ acquire(&agent_file_lock); file_cache_upsert_locked(ip, path); release(&agent_file_lock); return; }
  if(ip->type != T_DIR) return;
  for(off = 0; off + sizeof(de) <= ip->size; off += sizeof(de)){
    struct inode *child; char child_name[DIRSIZ + 1], child_path[AGENT_FILE_PATH_MAX];
    if(readi(ip, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) break;
    if(de.inum == 0) continue;
    if(namecmp(de.name, ".") == 0 || namecmp(de.name, "..") == 0) continue;
    memset(child_name, 0, sizeof(child_name)); memmove(child_name, de.name, DIRSIZ);
    if(file_join_path(path, child_name, child_path, sizeof(child_path)) < 0) continue;
    child = dirlookup(ip, child_name, 0);
    if(child == 0) continue;
    ilock(child); file_index_walk(child, child_path); iunlockput(child);
  }
}

static void file_index_build(void)
{
  struct inode *root;
  agent_file_init();
  begin_op();
  root = namei("/");
  if(root == 0){ end_op(); return; }
  ilock(root);
  acquire(&agent_file_lock);
  memset(file_meta, 0, sizeof(file_meta));
  file_index_reset_locked(); release(&agent_file_lock);
  file_index_walk(root, "");
  iunlockput(root);
  acquire(&agent_file_lock); file_rebuild_postings_locked(); agent_file_index_ready = 1; release(&agent_file_lock);
  end_op();
}

static void file_index_ensure(void)
{
  agent_file_init();
  acquire(&agent_file_lock);
  if(agent_file_index_ready){ release(&agent_file_lock); return; }
  release(&agent_file_lock);
  file_index_build();
}

static int file_query_parse(const char *params, struct agent_file_query_cond *conds, int *cond_count, char *keyword, int keyword_sz, int *force_scan)
{
  const char *p = params;
  *cond_count = 0; *force_scan = 0;
  if(keyword_sz > 0) keyword[0] = 0;
  while(*p){
    char key[INODE_ATTR_KEY_MAX], value[INODE_ATTR_VALUE_MAX]; int kn = 0, vn = 0;
    while(*p == ';') p++;
    if(*p == 0) break;
    while(*p && *p != '=' && *p != ';' && kn < (int)sizeof(key) - 1)
      key[kn++] = *p++;
    key[kn] = 0;
    if(*p != '=')
      return -1;
    p++;
    while(*p && *p != ';' && vn < (int)sizeof(value) - 1)
      value[vn++] = *p++;
    value[vn] = 0;
    if(streq(key, "keyword")){ safestrcpy(keyword, value, keyword_sz); }
    else if(streq(key, "public")){ ; }
    else if(streq(key, "mode")){ if(streq(value, "scan")) *force_scan = 1; }
    else if(streq(key, "nonce")){ ; }
    else if(*cond_count < AGENT_FILE_QUERY_COND_MAX){
      safestrcpy(conds[*cond_count].key, key, sizeof(conds[*cond_count].key));
      safestrcpy(conds[*cond_count].value, value, sizeof(conds[*cond_count].value)); (*cond_count)++;
    } else { return -1; }
    if(*p == ';') p++;
  }
  return 0;
}

static int file_meta_matches(struct agent_file_meta *meta, struct agent_file_query_cond *conds, int cond_count, const char *keyword)
{
  for(int i = 0; i < cond_count; i++){ if(!file_attr_match(meta->attrs, meta->attr_count, conds[i].key, conds[i].value)) return 0; }
  if(keyword[0] && !str_contains(meta->summary, keyword)) return 0;
  return 1;
}

static void append_file_result(char **ptr, int *left, struct agent_file_meta *meta)
{
  buf_puts(ptr, left, "{path="); buf_puts(ptr, left, meta->path);
  for(int i = 0; i < meta->attr_count; i++){
    buf_putc(ptr, left, ','); buf_puts(ptr, left, meta->attrs[i].key);
    buf_putc(ptr, left, '='); buf_puts(ptr, left, meta->attrs[i].value);
  }
  buf_puts(ptr, left, ",summary="); buf_puts(ptr, left, meta->summary); buf_putc(ptr, left, '}');
}

static int query_allows_public_share(const char *params)
{
  char value[INODE_ATTR_VALUE_MAX];
  if(param_value(params, "public", value, sizeof(value)) == 0 && streq(value, "true")) return 1;
  if(param_value(params, "owner", value, sizeof(value)) == 0 && streq(value, "system")) return 1;
  return 0;
}

static int cache_access_allowed(struct proc *p, struct shared_query_cache *entry)
{
  if(query_allows_public_share(entry->query)) return 1;
  return p->agent_group != 0 && p->agent_group == entry->owner_group;
}

static void query_result_with_cache(char *dst, int dstsz, const char *base, int hit, int owner_pid, int refcnt, uint64 version, int fs_scanned)
{
  char *ptr = dst; int left = dstsz; int n = strlen(base);
  memset(dst, 0, dstsz);
  if(n > 0 && base[n - 1] == '}') n--;
  for(int i = 0; i < n; i++) buf_putc(&ptr, &left, base[i]);
  buf_puts(&ptr, &left, ",cache_hit="); buf_putu(&ptr, &left, hit);
  buf_puts(&ptr, &left, ",cache_owner="); buf_putu(&ptr, &left, owner_pid);
  buf_puts(&ptr, &left, ",cache_refcnt="); buf_putu(&ptr, &left, refcnt);
  buf_puts(&ptr, &left, ",cache_version="); buf_putu(&ptr, &left, version);
  buf_puts(&ptr, &left, ",fs_scanned="); buf_putu(&ptr, &left, fs_scanned);
  buf_putc(&ptr, &left, '}');
}

// 修改点 #8: 增强的 shared query cache lookup (带统计)
static int shared_query_cache_lookup(struct proc *p, const char *query, struct agent_tool_response *resp)
{
  char *result;
  result = kalloc();
  if(result == 0){ tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return 1; }

  agent_fs_runtime_init();
  acquire(&agent_fs_runtime_lock);
  for(int i = 0; i < AGENT_SHARED_QUERY_CACHE_MAX; i++){
    struct shared_query_cache *entry = &shared_query_cache[i];
    if(!entry->used || entry->version != agent_file_version || !streq(entry->query, query) || !cache_access_allowed(p, entry))
      continue;
    entry->refcnt++; shared_cache_hits++;
    query_result_with_cache(result, AGENT_TOOL_RESULT_MAX, entry->result, 1, entry->owner_pid, entry->refcnt, entry->version, 0);
    release(&agent_fs_runtime_lock);
    tool_resp_set(resp, AGENT_TOOL_OK, result); kfree(result); return 1;
  }
  shared_cache_misses++;
  release(&agent_fs_runtime_lock);
  kfree(result); return 0;
}

static void shared_query_cache_store(struct proc *p, const char *query, const char *result)
{
  struct shared_query_cache *slot = 0;
  agent_fs_runtime_init();
  acquire(&agent_fs_runtime_lock);
  for(int i = 0; i < AGENT_SHARED_QUERY_CACHE_MAX; i++){
    if(shared_query_cache[i].used && streq(shared_query_cache[i].query, query)){ slot = &shared_query_cache[i]; break; }
    if(!shared_query_cache[i].used && slot == 0) slot = &shared_query_cache[i];
  }
  if(slot == 0) slot = &shared_query_cache[0];
  memset(slot, 0, sizeof(*slot)); slot->used = 1;
  safestrcpy(slot->query, query, sizeof(slot->query));
  safestrcpy(slot->result, result, sizeof(slot->result));
  slot->owner_pid = p->pid; slot->owner_group = p->agent_group; slot->refcnt = 1; slot->version = agent_file_version;
  release(&agent_fs_runtime_lock);
}

static void agent_file_version_bump(void)
{ agent_fs_runtime_init(); acquire(&agent_fs_runtime_lock); agent_file_version++; release(&agent_fs_runtime_lock); }

static void inode_summary_refresh(struct inode *ip)
{
  int n; memset(ip->summary, 0, sizeof(ip->summary));
  n = readi(ip, 0, (uint64)ip->summary, 0, sizeof(ip->summary) - 1);
  if(n < 0)
    n = 0;
  ip->summary[n] = 0;
}

void agentfs_content_changed(void)
{
  file_index_build();
  agent_file_version_bump();
  agent_signal_filemod();
}

void agentfs_tool_set_file_attr(struct agent_tool_request *req, struct agent_tool_response *resp)
{
  char path[AGENT_FILE_PATH_MAX], key[INODE_ATTR_KEY_MAX], value[INODE_ATTR_VALUE_MAX];
  struct inode *ip; int set = 0;
  if(param_value(req->params, "path", path, sizeof(path)) < 0 || param_value(req->params, "key", key, sizeof(key)) < 0 || param_value(req->params, "value", value, sizeof(value)) < 0)
  { tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return; }
  begin_op(); ip = namei(path);
  if(ip == 0){ end_op(); tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file not found"); return; }
  ilock(ip);
  if(ip->type != T_FILE){ iunlockput(ip); end_op(); tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "not a file"); return; }
  for(int i = 0; i < ip->attr_count; i++){
    if(streq(ip->attrs[i].key, key)){ safestrcpy(ip->attrs[i].value, value, sizeof(ip->attrs[i].value)); set = 1; break; }
  }
  if(!set){ if(ip->attr_count >= INODE_ATTR_MAX){ iunlockput(ip); end_op(); tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "too many attrs"); return; }
    safestrcpy(ip->attrs[ip->attr_count].key, key, sizeof(ip->attrs[ip->attr_count].key));
    safestrcpy(ip->attrs[ip->attr_count].value, value, sizeof(ip->attrs[ip->attr_count].value)); ip->attr_count++; }
  inode_summary_refresh(ip); iupdate(ip); iunlockput(ip); end_op();
  file_index_build(); agent_file_version_bump(); agent_signal_filemod();
  tool_resp_set(resp, AGENT_TOOL_OK, "attr set");
}

void agentfs_tool_get_file_attr(struct agent_tool_request *req, struct agent_tool_response *resp)
{
  char path[AGENT_FILE_PATH_MAX], key[INODE_ATTR_KEY_MAX], *buf; struct inode *ip;
  if(param_value(req->params, "path", path, sizeof(path)) < 0 || param_value(req->params, "key", key, sizeof(key)) < 0)
  { tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return; }
  buf = kalloc(); if(buf == 0){ tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return; }
  begin_op(); ip = namei(path);
  if(ip == 0){ end_op(); kfree(buf); tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file not found"); return; }
  ilock(ip);
  for(int i = 0; i < ip->attr_count; i++){
    if(streq(ip->attrs[i].key, key)){
      char *ptr = buf; int left = AGENT_TOOL_RESULT_MAX;
      memset(buf, 0, AGENT_TOOL_RESULT_MAX);
      buf_puts(&ptr, &left, "{status=ok,path="); buf_puts(&ptr, &left, path);
      buf_putc(&ptr, &left, ','); buf_puts(&ptr, &left, key); buf_putc(&ptr, &left, '=');
      buf_puts(&ptr, &left, ip->attrs[i].value); buf_putc(&ptr, &left, '}');
      iunlockput(ip); end_op(); tool_resp_set(resp, AGENT_TOOL_OK, buf); kfree(buf); return;
    }
  }
  iunlockput(ip); end_op(); kfree(buf); tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "attr not found");
}

void agentfs_tool_del_file_attr(struct agent_tool_request *req, struct agent_tool_response *resp)
{
  char path[AGENT_FILE_PATH_MAX], key[INODE_ATTR_KEY_MAX]; struct inode *ip;
  if(param_value(req->params, "path", path, sizeof(path)) < 0 || param_value(req->params, "key", key, sizeof(key)) < 0)
  { tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); return; }
  begin_op(); ip = namei(path);
  if(ip == 0){ end_op(); tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "file not found"); return; }
  ilock(ip);
  for(int i = 0; i < ip->attr_count; i++){
    if(streq(ip->attrs[i].key, key)){
      for(int j = i + 1; j < ip->attr_count; j++) ip->attrs[j - 1] = ip->attrs[j];
      memset(&ip->attrs[ip->attr_count - 1], 0, sizeof(ip->attrs[0])); ip->attr_count--;
      iupdate(ip); iunlockput(ip); end_op();
      file_index_build(); agent_file_version_bump(); agent_signal_filemod();
      tool_resp_set(resp, AGENT_TOOL_OK, "attr deleted"); return;
    }
  }
  iunlockput(ip); end_op(); tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "attr not found");
}

void agentfs_tool_query_file(struct proc *p, struct agent_tool_request *req, struct agent_tool_response *resp)
{
  struct agent_file_query_cond conds[AGENT_FILE_QUERY_COND_MAX];
  char keyword[INODE_ATTR_VALUE_MAX], *buf, *result, *ptr;
  int left = AGENT_TOOL_RESULT_MAX, cond_count, force_scan, count = 0, index_scanned = 0, full_scanned = 0, used_index = 0;
  uint64 ticks_begin, ticks_end;

  buf = kalloc(); result = kalloc();
  if(buf == 0 || result == 0){ if(buf) kfree(buf); if(result) kfree(result); tool_resp_set(resp, AGENT_TOOL_ERR_NO_SPACE, "no memory"); return; }
  ptr = buf; left = AGENT_TOOL_RESULT_MAX;

  if(shared_query_cache_lookup(p, req->params, resp)){
    agent_workflow_metric_query(p, 0, 0, 1);
    goto done;
  }
  file_index_ensure();
  if(file_query_parse(req->params, conds, &cond_count, keyword, sizeof(keyword), &force_scan) < 0)
  { tool_resp_set(resp, AGENT_TOOL_ERR_BAD_PARAM, "bad params"); goto done; }
  memset(buf, 0, AGENT_TOOL_RESULT_MAX);
  buf_puts(&ptr, &left, "{status=ok,files=[");
  acquire(&agent_file_lock);
  ticks_begin = agent_now_safe();
  for(int i = 0; i < AGENT_FILE_META_MAX; i++) if(file_meta[i].used) full_scanned++;
  if(cond_count > 0 && !force_scan){
    uint h = file_attr_hash(conds[0].key, conds[0].value);
    for(int post = file_index[h]; post >= 0; post = file_postings[post].next){
      struct agent_file_meta *meta; index_scanned++;
      if(!streq(file_postings[post].key, conds[0].key) || !streq(file_postings[post].value, conds[0].value)) continue;
      if(file_postings[post].entry_idx < 0 || file_postings[post].entry_idx >= AGENT_FILE_META_MAX) continue;
      meta = &file_meta[file_postings[post].entry_idx];
      if(!meta->used) continue;
      if(file_meta_matches(meta, conds, cond_count, keyword)){
        if(count < AGENT_FILE_RESULT_MAX){ if(count > 0) buf_putc(&ptr, &left, ','); append_file_result(&ptr, &left, meta); } count++;
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
        if(count < AGENT_FILE_RESULT_MAX){ if(count > 0) buf_putc(&ptr, &left, ','); append_file_result(&ptr, &left, meta); } count++;
      }
    }
  }
  ticks_end = agent_now_safe();
  release(&agent_file_lock);
  buf_puts(&ptr, &left, "],count="); buf_putu(&ptr, &left, count);
  buf_puts(&ptr, &left, ",used_index="); buf_putu(&ptr, &left, used_index);
  buf_puts(&ptr, &left, ",index_scanned="); buf_putu(&ptr, &left, index_scanned);
  buf_puts(&ptr, &left, ",full_scanned="); buf_putu(&ptr, &left, full_scanned);
  buf_puts(&ptr, &left, ",ticks_cost="); buf_putu(&ptr, &left, ticks_end - ticks_begin);
  // 修改点 #8: 查询计划说明
  buf_puts(&ptr, &left, ",query_plan=");
  if(used_index) buf_puts(&ptr, &left, "index"); else buf_puts(&ptr, &left, "scan");
  buf_puts(&ptr, &left, ",plan_reason=");
  buf_puts(&ptr, &left, force_scan ? "forced_scan" : (used_index ? "indexed_key" : "no_index_key"));
  buf_putc(&ptr, &left, '}');
  shared_query_cache_store(p, req->params, buf);
  query_result_with_cache(result, AGENT_TOOL_RESULT_MAX, buf, 0, p->pid, 1, agent_file_version, full_scanned);
  tool_resp_set(resp, AGENT_TOOL_OK, result);
  agent_workflow_metric_query(p, index_scanned,
                              used_index ? index_scanned : 0, 0);
done:
  kfree(buf); kfree(result);
}
