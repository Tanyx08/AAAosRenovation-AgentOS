#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/agent.h"
#include "user/user.h"

static int failures;

struct lease_child_result {
  int ret;
  uint64 lease_id;
};

static void
check(int ok, const char *msg)
{
  if(ok){
    printf("agentleasetest: ok: %s\n", msg);
  } else {
    printf("agentleasetest: FAIL: %s\n", msg);
    failures++;
  }
}

static void
make_exact_file(const char *path, const char *data)
{
  int fd;

  unlink(path);
  fd = open(path, O_CREATE | O_RDWR);
  check(fd >= 0, "create lease test file");
  if(fd < 0)
    return;
  write(fd, data, strlen(data));
  close(fd);
}

static int
read_file_into(const char *path, char *buf, int max)
{
  int fd;
  int n;

  if(max <= 0)
    return -1;
  memset(buf, 0, max);
  fd = open(path, O_RDONLY);
  if(fd < 0)
    return -1;
  n = read(fd, buf, max - 1);
  close(fd);
  if(n < 0)
    return -1;
  buf[n] = 0;
  return n;
}

static int
starts_with(const char *s, const char *prefix)
{
  while(*prefix){
    if(*s != *prefix)
      return 0;
    s++;
    prefix++;
  }
  return 1;
}

static uint64
parse_lease_id(const char *s)
{
  const char *p = s;
  uint64 value = 0;

  while(*p){
    if(starts_with(p, "lease_id=")){
      p += 9;
      while(*p >= '0' && *p <= '9'){
        value = value * 10 + *p - '0';
        p++;
      }
      return value;
    }
    p++;
  }
  return 0;
}

static int
begin_lease(const char *path, uint64 *lease_id)
{
  char result[64];
  int ret;

  memset(result, 0, sizeof(result));
  ret = agent_lease_begin(path, result);
  if(ret == AGENT_TOOL_OK && lease_id != 0)
    *lease_id = parse_lease_id(result);
  return ret;
}

static void
same_inode_conflict_test(void)
{
  struct lease_child_result child_result;
  uint64 lease_id = 0;
  int pfd[2];
  int child;
  int status = -1;

  make_exact_file("lease_same", "same");
  check(begin_lease("lease_same", &lease_id) == AGENT_TOOL_OK &&
        lease_id != 0, "owner begins lease on inode");
  check(pipe(pfd) == 0, "create conflict result pipe");

  child = fork();
  if(child == 0){
    close(pfd[0]);
    agent_create(AGENT_TYPE_PRIMARY, 0, 256);
    memset(&child_result, 0, sizeof(child_result));
    child_result.ret = begin_lease("lease_same", &child_result.lease_id);
    write(pfd[1], &child_result, sizeof(child_result));
    close(pfd[1]);
    exit(0);
  }

  close(pfd[1]);
  memset(&child_result, 0, sizeof(child_result));
  check(read(pfd[0], &child_result, sizeof(child_result)) ==
        sizeof(child_result), "read conflict result");
  close(pfd[0]);
  check(wait(&status) == child && status == 0, "conflict child exits cleanly");
  check(child_result.ret == AGENT_TOOL_ERR_BUSY,
        "same inode second lease returns BUSY");
  check(agent_lease_abort(lease_id) == AGENT_TOOL_OK,
        "owner aborts conflicting lease");
}

static void
different_inode_parallel_test(void)
{
  struct lease_child_result child_result;
  uint64 lease_id = 0;
  int pfd[2];
  int child;
  int status = -1;

  make_exact_file("lease_a", "a");
  make_exact_file("lease_b", "b");
  check(begin_lease("lease_a", &lease_id) == AGENT_TOOL_OK &&
        lease_id != 0, "owner begins lease on first inode");
  check(pipe(pfd) == 0, "create parallel result pipe");

  child = fork();
  if(child == 0){
    close(pfd[0]);
    agent_create(AGENT_TYPE_PRIMARY, 0, 256);
    memset(&child_result, 0, sizeof(child_result));
    child_result.ret = begin_lease("lease_b", &child_result.lease_id);
    if(child_result.ret == AGENT_TOOL_OK && child_result.lease_id != 0)
      agent_lease_abort(child_result.lease_id);
    write(pfd[1], &child_result, sizeof(child_result));
    close(pfd[1]);
    exit(0);
  }

  close(pfd[1]);
  memset(&child_result, 0, sizeof(child_result));
  check(read(pfd[0], &child_result, sizeof(child_result)) ==
        sizeof(child_result), "read parallel result");
  close(pfd[0]);
  check(wait(&status) == child && status == 0, "parallel child exits cleanly");
  check(child_result.ret == AGENT_TOOL_OK && child_result.lease_id != 0,
        "different inode leases can run in parallel");
  check(agent_lease_abort(lease_id) == AGENT_TOOL_OK,
        "owner aborts first inode lease");
}

static void
non_owner_commit_abort_test(void)
{
  struct lease_child_result child_result;
  uint64 lease_id = 0;
  int pfd[2];
  int child;
  int status = -1;

  make_exact_file("lease_owner", "owner");
  check(begin_lease("lease_owner", &lease_id) == AGENT_TOOL_OK &&
        lease_id != 0, "owner begins protected lease");
  check(pipe(pfd) == 0, "create non-owner result pipe");

  child = fork();
  if(child == 0){
    close(pfd[0]);
    agent_create(AGENT_TYPE_PRIMARY, 0, 256);
    memset(&child_result, 0, sizeof(child_result));
    child_result.ret = agent_lease_commit(lease_id, 0);
    child_result.lease_id = agent_lease_abort(lease_id);
    write(pfd[1], &child_result, sizeof(child_result));
    close(pfd[1]);
    exit(0);
  }

  close(pfd[1]);
  memset(&child_result, 0, sizeof(child_result));
  check(read(pfd[0], &child_result, sizeof(child_result)) ==
        sizeof(child_result), "read non-owner result");
  close(pfd[0]);
  check(wait(&status) == child && status == 0, "non-owner child exits cleanly");
  check(child_result.ret == AGENT_TOOL_ERR_PERMISSION,
        "non-owner cannot commit lease");
  check((int)child_result.lease_id == AGENT_TOOL_ERR_PERMISSION,
        "non-owner cannot abort lease");
  check(agent_lease_abort(lease_id) == AGENT_TOOL_OK,
        "owner can still abort after rejected non-owner operations");
}

static void
exit_reap_test(void)
{
  struct lease_child_result child_result;
  uint64 lease_id = 0;
  int pfd[2];
  int child;
  int status = -1;

  make_exact_file("lease_exit", "exit");
  check(pipe(pfd) == 0, "create exit reap pipe");
  child = fork();
  if(child == 0){
    close(pfd[0]);
    agent_create(AGENT_TYPE_PRIMARY, 0, 256);
    memset(&child_result, 0, sizeof(child_result));
    child_result.ret = begin_lease("lease_exit", &child_result.lease_id);
    write(pfd[1], &child_result, sizeof(child_result));
    close(pfd[1]);
    exit(0);
  }

  close(pfd[1]);
  memset(&child_result, 0, sizeof(child_result));
  check(read(pfd[0], &child_result, sizeof(child_result)) ==
        sizeof(child_result), "read exiting owner lease result");
  close(pfd[0]);
  check(child_result.ret == AGENT_TOOL_OK && child_result.lease_id != 0,
        "child begins lease before exit");
  check(wait(&status) == child && status == 0, "lease owner child exits");
  check(begin_lease("lease_exit", &lease_id) == AGENT_TOOL_OK &&
        lease_id != 0, "lease is reaped after owner exit");
  check(agent_lease_abort(lease_id) == AGENT_TOOL_OK,
        "abort lease acquired after owner exit");
}

static void
abort_no_file_change_test(void)
{
  char buf[64];
  uint64 lease_id = 0;

  make_exact_file("lease_abort", "before");
  check(begin_lease("lease_abort", &lease_id) == AGENT_TOOL_OK &&
        lease_id != 0, "begin lease before abort");
  check(agent_lease_abort(lease_id) == AGENT_TOOL_OK,
        "abort lease succeeds");
  check(read_file_into("lease_abort", buf, sizeof(buf)) > 0 &&
        strcmp(buf, "before") == 0,
        "abort leaves file content unchanged");
  lease_id = 0;
  check(begin_lease("lease_abort", &lease_id) == AGENT_TOOL_OK &&
        lease_id != 0, "lease can be reacquired after abort");
  check(agent_lease_abort(lease_id) == AGENT_TOOL_OK,
        "abort reacquired lease");
}

int
main(void)
{
  check((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 512) > 0,
        "agent_create for lease test");

  same_inode_conflict_test();
  different_inode_parallel_test();
  non_owner_commit_abort_test();
  exit_reap_test();
  abort_no_file_change_test();

  if(failures){
    printf("agentleasetest: %d failures\n", failures);
    exit(1);
  }
  printf("agentleasetest: all tests passed\n");
  exit(0);
}
