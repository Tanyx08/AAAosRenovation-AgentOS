// Saved registers for kernel context switches.
#include "agent.h"

struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?

  // 修改点 #12: 软预算调度统计
  int agent_burst_count;       // 连续 Agent 运行计数
  int normal_burst_count;      // 连续普通进程运行计数
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

#define MAX_VMA_COUNT 16
#define VMA_START (MAXVA / 2)
struct vma {
  uint64 start;
  uint64 end;
  uint64 length; // 0 means vma not used
  uint64 off;
  int permission;
  int flags;
  struct file *file;
  struct vma *next;

  struct spinlock lock;
};

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  struct vma *vma; // virtual memory area

  // ---- Agent 身份与生命周期 ----
  int agent_type;
  int agent_role;              // 修改点 #3: 角色 (Planner/Retriever/Patch/Test/Reviewer)
  int agent_role_locked;       // 角色只允许在创建阶段设置一次
  int heartbeat_interval;
  uint64 resource_quota;
  int loop_state;
  uint64 context_region_start;
  uint64 context_region_size;
  uint64 context_path_len;
  uint64 context_node_count;
  uint64 context_dropped_nodes;
  uint64 heartbeat_deadline;
  int heartbeat_wheel_next;
  int heartbeat_wheel_bucket;
  int heartbeat_wheel_active;
  uint64 wakeup_tick;
  uint64 runnable_since;
  uint16 context_offsets[AGENT_CONTEXT_MAX_NODES];
  uint16 context_lengths[AGENT_CONTEXT_MAX_NODES];
  int watch_mask;
  int pending_events;
  int last_wakeup_reason;

  // ---- 权限与身份 (修改点 #3) ----
  int agent_priority;
  int agent_group;
  uint64 agent_capabilities;   // 修改点 #3: 轻量 capability 位掩码
  uint64 workflow_id;          // 修改点 #4: 工作流 ID
  int workflow_leader_pid;      // 工作流 leader PID
  uint64 workflow_leader_generation; // leader 身份代次，防 PID 复用
  int agent_parent_pid;         // Agent 父进程 PID
  uint64 agent_parent_generation; // Agent 父进程身份代次
  int is_workflow_leader;       // 只有真正的 Primary/Planner 持有
  int workflow_member_registered; // 是否已计入 workflow 表
  uint64 identity_generation;  // 修改点 #4: 身份代次 (防 PID 复用)

  // ---- FIFO 邮箱 (修改点 #1) ----
  struct agent_mailbox mailbox; // 替换原来的 agent_message[AGENT_MESSAGE_MAX]

  // ---- 文件监听 (修改点 #16) ----
  uint agent_watch_dev;
  uint agent_watch_inum;
  char agent_watch_path[AGENT_MESSAGE_MAX];

  // ---- 调度参数 (修改点 #12) ----
  int agent_sched_priority;
  int agent_sched_quota;
  int agent_sched_budget;
  int agent_sched_boost;
  uint64 agent_sched_vruntime;   // 修改点 #12: 虚拟运行时间
  uint64 agent_sched_last_run;   // 修改点 #12: 上次运行 tick
  int agent_sched_budget_penalty;// 修改点 #12: 预算惩罚计数

  // ---- Context ABI (修改点 #7) ----
  uint64 context_generation;     // 修改点 #7: Context 写代次
  uint64 context_first_sequence; // 修改点 #7: 首个节点序列号
  uint64 context_next_sequence;  // 修改点 #7: 下一个序列号

  // ---- 内核可信摘要 (修改点 #6) ----
  struct agent_context_digest context_digests[AGENT_CONTEXT_DIGEST_MAX];
  int context_digest_head;
  int context_digest_count;

  // ---- agent_wait 超时 (修改点 #17) ----
  uint64 wait_generation;        // 等待代次（防旧超时重复唤醒）
  int wait_timeout_ticks;        // 超时 tick 数
  uint64 wait_deadline;          // 超时截止 tick

  // ---- Context 复用 (修改点 #8) ----
  uint64 last_context_query;     // 上次查询的 hash
  int context_reuse_hit;         // 复用命中次数

  // ---- 调度软预算补充周期 (修改点 #12) ----
  uint64 budget_replenish_deadline;
};

// ---- 全局 Agent 运行时状态 ----
struct agent_global_state {
  struct spinlock lock;
  int ready;

  // 修改点 #2: 文件编辑租约表
  struct agent_edit_lease leases[AGENT_LEASE_MAX];
  uint64 next_lease_id;
  uint64 next_identity_generation;

  // 修改点 #24: 心跳时间轮
  int heartbeat_wheel_heads[AGENT_HEARTBEAT_WHEEL_BUCKETS];
  int heartbeat_wheel_pids[AGENT_HEARTBEAT_WHEEL_BUCKETS * 4]; // 每桶最多4个
  uint64 heartbeat_min_deadline;
  int heartbeat_count;
  uint64 heartbeat_tick_scanned;
  uint64 heartbeat_tick_wakeups;

  // Workflow 表用于级联终止和孤儿 Agent 收敛。
  struct agent_workflow workflows[AGENT_WORKFLOW_MAX];
};
