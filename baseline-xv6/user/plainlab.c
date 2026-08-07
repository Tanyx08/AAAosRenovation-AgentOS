#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "user/dual_result.h"

#define MAX_CONTENT 2048
#define MAX_PATCHED 2304

static int files_scanned;
static int syscalls_count;
static int polling_loops;
static int duplicate_queries;
static char scan_content[MAX_CONTENT];
static char patch_content[MAX_CONTENT];
static char patched_content[MAX_PATCHED];
static char test_todo[MAX_CONTENT];
static char test_src[MAX_CONTENT];

static int
contains(const char *s, const char *needle)
{
  int n = strlen(needle);
  int i;

  if(n == 0)
    return 1;
  for(; *s; s++){
    for(i = 0; i < n && s[i] == needle[i]; i++)
      ;
    if(i == n)
      return 1;
  }
  return 0;
}

static int
find_substr(const char *s, const char *needle)
{
  int n = strlen(needle);
  int i;
  int j;

  if(n == 0)
    return 0;
  for(i = 0; s[i]; i++){
    for(j = 0; j < n && s[i + j] == needle[j]; j++)
      ;
    if(j == n)
      return i;
  }
  return -1;
}

static void
copy_dir_name(char *dst, const char *src)
{
  int i;

  for(i = 0; i < DIRSIZ && src[i]; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

static void
join_repo_path(char *dst, const char *name)
{
  strcpy(dst, "repo/");
  strcpy(dst + strlen(dst), name);
}

static int
read_file(const char *path, char *buf, int max)
{
  int fd;
  int n;

  memset(buf, 0, max);
  fd = open(path, O_RDONLY);
  syscalls_count++;
  if(fd < 0)
    return -1;
  n = read(fd, buf, max - 1);
  syscalls_count++;
  close(fd);
  syscalls_count++;
  if(n < 0)
    return -1;
  buf[n] = 0;
  return n;
}

static int
write_file_replace(const char *path, const char *buf)
{
  int fd;
  int n = strlen(buf);

  unlink(path);
  syscalls_count++;
  fd = open(path, O_CREATE | O_RDWR);
  syscalls_count++;
  if(fd < 0)
    return -1;
  if(write(fd, buf, n) != n){
    syscalls_count++;
    close(fd);
    syscalls_count++;
    return -1;
  }
  syscalls_count++;
  close(fd);
  syscalls_count++;
  return 0;
}

static void
poll_plain_stage(void)
{
  int child;
  int fd;

  unlink("plain_stage");
  syscalls_count++;
  child = fork();
  syscalls_count++;
  if(child == 0){
    sleep(3);
    fd = open("plain_stage", O_CREATE | O_RDWR);
    if(fd >= 0){
      write(fd, "ready", 5);
      close(fd);
    }
    exit(0);
  }

  for(;;){
    fd = open("plain_stage", O_RDONLY);
    syscalls_count++;
    polling_loops++;
    if(fd >= 0){
      close(fd);
      syscalls_count++;
      break;
    }
    sleep(1);
  }
  wait(0);
  syscalls_count++;
}

static int
scan_repo_for_todo(char *target)
{
  char name[DIRSIZ + 1];
  char path[64];
  int fd;
  struct dirent de;
  struct stat st;

  fd = open("repo", O_RDONLY);
  syscalls_count++;
  if(fd < 0)
    return -1;

  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    syscalls_count++;
    if(de.inum == 0)
      continue;
    copy_dir_name(name, de.name);
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    join_repo_path(path, name);
    if(stat(path, &st) < 0){
      syscalls_count++;
      continue;
    }
    syscalls_count++;
    if(st.type != T_FILE)
      continue;
    files_scanned++;
    if(read_file(path, scan_content, sizeof(scan_content)) < 0)
      continue;
    if(contains(scan_content, "delete_task") &&
       (contains(scan_content, "missing task_count--") ||
        contains(scan_content, "task_count--;"))){
      strcpy(target, path);
      close(fd);
      syscalls_count++;
      return 0;
    }
  }
  close(fd);
  syscalls_count++;
  return -1;
}

static int
patch_todo_file(const char *path)
{
  const char *old = "  // BUG: missing task_count--";
  const char *new = "  task_count--;";
  int idx;
  int pos = 0;
  int i;

  if(read_file(path, patch_content, sizeof(patch_content)) < 0)
    return -1;
  idx = find_substr(patch_content, old);
  if(idx < 0)
    return contains(patch_content, "task_count--;") ? 0 : -1;

  memset(patched_content, 0, sizeof(patched_content));
  for(i = 0; i < idx && pos < sizeof(patched_content) - 1; i++)
    patched_content[pos++] = patch_content[i];
  for(i = 0; new[i] && pos < sizeof(patched_content) - 1; i++)
    patched_content[pos++] = new[i];
  i = idx + strlen(old);
  while(patch_content[i] && pos < sizeof(patched_content) - 1)
    patched_content[pos++] = patch_content[i++];
  patched_content[pos] = 0;

  return write_file_replace(path, patched_content);
}

static int
run_rule_test(const char *path)
{
  int pass = 0;

  if(read_file(path, test_todo, sizeof(test_todo)) < 0)
    return 0;
  if(read_file("repo/test.c", test_src, sizeof(test_src)) < 0)
    return 0;

  if(contains(test_todo, "task_count--;"))
    pass++;
  if(contains(test_todo, "delete_task"))
    pass++;
  if(contains(test_src, "delete_task") ||
     contains(test_src, "test_delete_updates_count"))
    pass++;
  return pass == 3;
}

int
main(void)
{
  char target[64];
  char initial_hash_hex[9];
  char final_hash_hex[9];
  uint32 initial_hash = 0;
  uint32 final_hash = 0;
  int initial_size = 0;
  int final_size = 0;
  int final_content_size;
  int start;
  int end;
  int found;
  int patch_ok;
  int test_ok;
  int review_ok;
  int initial_digest_ok;
  int digest_ok;

  memset(target, 0, sizeof(target));
  memset(initial_hash_hex, 0, sizeof(initial_hash_hex));
  memset(final_hash_hex, 0, sizeof(final_hash_hex));
  initial_digest_ok =
    dual_file_digest("repo/todo.c", &initial_hash, &initial_size) == 0;
  if(initial_digest_ok)
    dual_hash_hex(initial_hash, initial_hash_hex);
  start = uptime();
  poll_plain_stage();

  found = scan_repo_for_todo(target) == 0;
  duplicate_queries++;
  if(found){
    char duplicate[64];

    memset(duplicate, 0, sizeof(duplicate));
    scan_repo_for_todo(duplicate);
  }
  patch_ok = found && patch_todo_file(target) == 0;
  test_ok = patch_ok && run_rule_test(target);
  end = uptime();
  digest_ok = found &&
              dual_file_digest(target, &final_hash, &final_size) == 0;
  if(digest_ok)
    dual_hash_hex(final_hash, final_hash_hex);
  final_content_size = found ?
    dual_read_file(target, test_todo, sizeof(test_todo)) : -1;
  patch_ok = patch_ok && final_content_size == final_size &&
             dual_validate_todo(test_todo);
  review_ok = found && patch_ok && test_ok && initial_digest_ok && digest_ok;

  printf("[PLAIN] task=fix_todo_delete status=%s\n",
         review_ok ? "PASS" : "FAIL");
  printf("[PLAIN] file=%s found=%d\n", target, found);
  printf("[PLAIN] patch=task_count-- status=%s\n",
         patch_ok ? "PASS" : "FAIL");
  printf("[PLAIN] test=rule_test status=%s\n",
         test_ok ? "PASS" : "FAIL");
  printf("[PLAIN] review=final_file status=%s\n",
         review_ok ? "PASS" : "FAIL");
  printf("[RESULT] suite=dual target=plain found=%d patch_ok=%d test_ok=%d review_ok=%d initial_size=%d initial_hash=%s file_size=%d file_hash=%s status=%s\n",
         found, patch_ok, test_ok, review_ok, initial_size, initial_hash_hex,
         final_size, final_hash_hex, review_ok ? "PASS" : "FAIL");
  printf("[METRIC] suite=dual target=plain total_ticks=%d files_scanned=%d syscalls=%d polling_loops=%d duplicate_queries=%d found=%d patch_ok=%d test_ok=%d review_ok=%d status=%s\n",
         (int)(end - start), files_scanned, syscalls_count, polling_loops,
         duplicate_queries, found, patch_ok, test_ok, review_ok,
         review_ok ? "PASS" : "FAIL");

  exit(review_ok ? 0 : 1);
}
