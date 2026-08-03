#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in case of overflow
    return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);
extern uint64 sys_agent_create(void);
extern uint64 sys_agent_info(void);
extern uint64 sys_tool_call(void);
extern uint64 sys_tool_list(void);
extern uint64 sys_context_push(void);
extern uint64 sys_context_query(void);
extern uint64 sys_context_rollback(void);
extern uint64 sys_context_clear(void);
extern uint64 sys_agent_heartbeat_set(void);
extern uint64 sys_agent_heartbeat_stop(void);
extern uint64 sys_agent_watch(void);
extern uint64 sys_agent_wait(void);
extern uint64 sys_agent_unwatch(void);
extern uint64 sys_agent_priority_set(void);
extern uint64 sys_tool_register(void);
extern uint64 sys_tool_recv(void);
extern uint64 sys_tool_reply(void);
extern uint64 sys_agent_watch_file(void);
extern uint64 sys_agent_sched_set(void);
extern uint64 sys_agent_wait_timeout(void);
extern uint64 sys_tool_call_batch(void);
extern uint64 sys_tool_schema(void);
extern uint64 sys_agent_cap_set(void);
extern uint64 sys_agent_lease_begin(void);
extern uint64 sys_agent_lease_commit(void);
extern uint64 sys_agent_lease_abort(void);
extern uint64 sys_agent_query_agent(void);
extern uint64 sys_agent_role_set(void);
extern uint64 sys_agent_context_verify(void);
extern uint64 sys_agent_cascade_kill(void);

// An array mapping syscall numbers from syscall.h
// to the function that handles the system call.
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_sleep]   sys_sleep,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
[SYS_mmap]    sys_mmap,
[SYS_munmap]  sys_munmap,
[SYS_agent_create] sys_agent_create,
[SYS_agent_info] sys_agent_info,
[SYS_tool_call] sys_tool_call,
[SYS_tool_list] sys_tool_list,
[SYS_context_push] sys_context_push,
[SYS_context_query] sys_context_query,
[SYS_context_rollback] sys_context_rollback,
[SYS_context_clear] sys_context_clear,
[SYS_agent_heartbeat_set] sys_agent_heartbeat_set,
[SYS_agent_heartbeat_stop] sys_agent_heartbeat_stop,
[SYS_agent_watch] sys_agent_watch,
[SYS_agent_wait] sys_agent_wait,
[SYS_agent_unwatch] sys_agent_unwatch,
[SYS_agent_priority_set] sys_agent_priority_set,
[SYS_tool_register] sys_tool_register,
[SYS_tool_recv] sys_tool_recv,
[SYS_tool_reply] sys_tool_reply,
[SYS_agent_watch_file] sys_agent_watch_file,
[SYS_agent_sched_set] sys_agent_sched_set,
[SYS_agent_wait_timeout] sys_agent_wait_timeout,
[SYS_tool_call_batch] sys_tool_call_batch,
[SYS_tool_schema] sys_tool_schema,
[SYS_agent_cap_set] sys_agent_cap_set,
[SYS_agent_lease_begin] sys_agent_lease_begin,
[SYS_agent_lease_commit] sys_agent_lease_commit,
[SYS_agent_lease_abort] sys_agent_lease_abort,
[SYS_agent_query_agent] sys_agent_query_agent,
[SYS_agent_role_set] sys_agent_role_set,
[SYS_agent_context_verify] sys_agent_context_verify,
[SYS_agent_cascade_kill] sys_agent_cascade_kill,
};

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // Use num to lookup the system call function for num, call it,
    // and store its return value in p->trapframe->a0
    p->trapframe->a0 = syscalls[num]();
  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
