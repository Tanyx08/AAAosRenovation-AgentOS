// Agent-OS 公共内核 ABI。
//
// 这个头文件定义了 Agent-OS 内核子系统与用户态 Agent 程序共享使用的
// 常量、结构体和函数声明。内容包括 Agent Context 布局、工具调用请求/
// 响应格式、等待事件结构、动态工具请求结构，以及 Agent 管理相关的
// 主要内核入口函数。
//
// 版本历史:
//   v1 - 初始版本
//   v2 - 决赛改进: FIFO 邮箱、capability 权限、Context ABI 固定、
//        文件编辑租约、tool schema、agent_wait timeout/cancel、
//        agent_spawn、tool_call_batch、统一日志字段、心跳时间轮、
//        Context 可信摘要、guard page、软预算调度

#ifndef XV6_AGENT_H
#define XV6_AGENT_H

#include "types.h"

// ---- Context ABI 常量 ----
#define AGENT_CONTEXT_REGION_SIZE (8192)
#define AGENT_CONTEXT_HEADER_MAGIC (0x41474e54U)  // "AGNT"
#define AGENT_CONTEXT_HEADER_VERSION (2U)          // v2: 增加 generation/guard page

// ---- Agent 类型 ----
#define AGENT_TYPE_NORMAL (0)
#define AGENT_TYPE_PRIMARY (1)
#define AGENT_TYPE_WORKER (2)

// ---- Agent Loop 状态 ----
#define AGENT_LOOP_IDLE (0)
#define AGENT_LOOP_READY (1)
#define AGENT_LOOP_RUNNING (2)
#define AGENT_LOOP_WAITING (3)
#define AGENT_LOOP_ROLLED_BACK (4)
#define AGENT_LOOP_DONE (5)

// ---- 事件类型 ----
#define AGENT_EVENT_NONE (0)
#define AGENT_EVENT_HEARTBEAT (1)
#define AGENT_EVENT_MESSAGE (2)
#define AGENT_EVENT_FILEMOD (4)

#define AGENT_WATCH_MESSAGE AGENT_EVENT_MESSAGE
#define AGENT_WATCH_FILEMOD AGENT_EVENT_FILEMOD

// ---- 工具标志 ----
#define AGENT_TOOL_FLAG_PUBLIC (1)

// ---- 容量常量 ----
#define AGENT_TOOL_NAME_MAX (32)
#define AGENT_TOOL_PARAM_MAX (128)
#define AGENT_TOOL_RESULT_MAX (512)
#define AGENT_CONTEXT_REQ_MAX (96)
#define AGENT_CONTEXT_RES_MAX (160)
#define AGENT_CONTEXT_MAX_NODES (16)
#define AGENT_MESSAGE_MAX (128)
#define AGENT_DYNAMIC_TOOL_MAX (8)
#define AGENT_DYNAMIC_REQUEST_MAX (8)

// ---- FIFO 邮箱 (修改点 #1) ----
#define AGENT_MAILBOX_CAP (8)         // 有界 FIFO 邮箱容量
#define AGENT_MESSAGE_TYPE_NORMAL (0)  // 普通用户消息
#define AGENT_MESSAGE_TYPE_SYSTEM (1)  // 系统事件消息 (FILEMOD/CANCEL 等)
#define AGENT_MAILBOX_SYSTEM_SLOTS (2) // 为系统事件预留的槽位

// ---- 文件编辑租约 (修改点 #2) ----
#define AGENT_LEASE_MAX (8)           // 全局最大租约数
#define AGENT_LEASE_EXPIRY_TICKS (200) // 租约默认过期时间(tick)

// ---- 权限模型 (修改点 #3) ----
#define AGENT_CAP_QUERY_PROCESS  (1ULL << 0)
#define AGENT_CAP_QUERY_FILE     (1ULL << 1)
#define AGENT_CAP_READ_FILE      (1ULL << 2)
#define AGENT_CAP_PATCH_FILE     (1ULL << 3)
#define AGENT_CAP_SEND_MESSAGE   (1ULL << 4)
#define AGENT_CAP_WATCH_FILE     (1ULL << 5)
#define AGENT_CAP_REGISTER_TOOL  (1ULL << 6)
#define AGENT_CAP_SCHED_CONFIG   (1ULL << 7)
#define AGENT_CAP_LEASE_ACQUIRE  (1ULL << 8)
#define AGENT_CAP_WORKFLOW_CTRL  (1ULL << 9)
#define AGENT_CAP_AUDIT_READ     (1ULL << 10)
#define AGENT_CAP_ALL            ((1ULL << 11) - 1)

// ---- Agent 角色 ----
#define AGENT_ROLE_UNSET         (-1)
#define AGENT_ROLE_PLANNER       (0)
#define AGENT_ROLE_RETRIEVER     (1)
#define AGENT_ROLE_PATCH         (2)
#define AGENT_ROLE_TEST          (3)
#define AGENT_ROLE_REVIEWER      (4)
#define AGENT_ROLE_TOOL_SERVICE  (5)

// ---- 可信 Context 摘要 (修改点 #6) ----
#define AGENT_CONTEXT_DIGEST_MAX (16)  // 内核可信摘要环容量

// ---- Context 复用与共享缓存 (修改点 #8) ----
#define AGENT_CONTEXT_REUSE_HIT (1)    // Context 命中标记

// ---- 心跳时间轮 (修改点 #24) ----
#define AGENT_HEARTBEAT_WHEEL_BUCKETS (64)  // 时间轮桶数
#define AGENT_HEARTBEAT_WHEEL_MASK (63)

// ---- 工具调用错误码 (修改点 #14) ----
#define AGENT_TOOL_OK (0)
#define AGENT_TOOL_ERR_TOOL_NOT_FOUND (-1)
#define AGENT_TOOL_ERR_BAD_PARAM (-2)
#define AGENT_TOOL_ERR_NOT_AGENT (-3)
#define AGENT_TOOL_ERR_NO_SPACE (-4)
#define AGENT_TOOL_ERR_BUSY (-5)
#define AGENT_TOOL_ERR_PERMISSION (-6)
#define AGENT_TOOL_ERR_SERVICE_GONE (-7)
#define AGENT_TOOL_ERR_TIMEOUT (-8)       // 新增: 请求超时
#define AGENT_TOOL_ERR_STALE_REQUEST (-9)  // 新增: 过期请求
#define AGENT_TOOL_ERR_CONFLICT (-10)      // 新增: 冲突(如租约冲突)
#define AGENT_TOOL_ERR_STALE (-11)         // 新增: 版本过期
#define AGENT_TOOL_ERR_NO_WAKE_SOURCE (-12) // 新增: 无唤醒源

// ---- 调度常量 (修改点 #12) ----
#define AGENT_SCHED_PRIORITY_MIN (1)
#define AGENT_SCHED_PRIORITY_MAX (8)
#define AGENT_SCHED_QUOTA_MIN (1)
#define AGENT_SCHED_QUOTA_MAX (8)
#define AGENT_SCHED_BUDGET_REPLENISH_INTERVAL (10) // budget 补充周期(tick)
#define AGENT_MAX_AGENT_BURST (8)                   // 最大连续Agent运行次数

// ---- agent_wait 返回码 (修改点 #17) ----
#define AGENT_WAIT_MESSAGE   (1)
#define AGENT_WAIT_FILEMOD   (2)
#define AGENT_WAIT_HEARTBEAT (3)
#define AGENT_WAIT_TIMEOUT   (4)
#define AGENT_WAIT_CANCELLED (5)
#define AGENT_WAIT_NO_SOURCE (6)

// ---- Tool Call batch (修改点 #22) ----
#define AGENT_TOOL_BATCH_MAX (8)  // 单次 batch 最大工具数

struct proc;

// ---- Agent 信息结构 ----
struct agent_info {
  uint64 context_start;
  uint64 context_size;
  int agent_type;
  int heartbeat_interval;
  uint64 resource_quota;
  int loop_state;
  uint64 context_path_len;
  uint64 context_node_count;
  uint64 dropped_nodes;
  int agent_priority;
  int agent_group;
  int sched_priority;
  int sched_quota;
  int sched_budget;
  // 修改点 #3: capability 字段
  uint64 agent_capabilities;
  // 修改点 #1: 邮箱统计
  uint64 mailbox_count;
  uint64 mailbox_dropped;
};

// ---- 工具调用请求/响应 (修改点 #14: 增加 version/request_id) ----
struct agent_tool_request {
  uint32 version;                           // ABI 版本
  uint64 request_id;                        // 请求 ID
  char tool[AGENT_TOOL_NAME_MAX];
  char params[AGENT_TOOL_PARAM_MAX];
};

struct agent_tool_response {
  int status;
  uint32 result_len;
  uint64 request_id;                        // 回显请求 ID
  char result[AGENT_TOOL_RESULT_MAX];
};

// ---- 工具 Schema 描述 (修改点 #14) ----
struct agent_tool_schema {
  char name[AGENT_TOOL_NAME_MAX];
  char params_desc[AGENT_TOOL_PARAM_MAX];   // "type:string?,owner:string?,..."
  uint64 required_cap;                      // 所需 capability
  char result_desc[AGENT_TOOL_RESULT_MAX];  // "paths[],cache_hit,..."
  char errors_desc[128];                    // "BAD_PARAM,PERMISSION,..."
  int is_dynamic;
  int owner_pid;
  int owner_group;
  int flags;
};

// ---- Tool Call batch (修改点 #22) ----
struct agent_tool_batch_request {
  int count;
  struct agent_tool_request requests[AGENT_TOOL_BATCH_MAX];
};

struct agent_tool_batch_response {
  int count;
  struct agent_tool_response responses[AGENT_TOOL_BATCH_MAX];
};

// ---- Context 结构 ----
struct agent_context_node {
  uint64 timestamp_ms;
  uint64 sequence;         // 修改点 #7: 单调序列号
  uint64 request_id;       // 修改点 #4/#19: 请求 ID
  uint64 span_id;          // 修改点 #4/#19: 工作流 span
  uint64 cause_sequence;   // 修改点 #4/#19: 因果链
  int status;              // 修改点 #7: 节点状态
  char request[AGENT_CONTEXT_REQ_MAX];
  char result[AGENT_CONTEXT_RES_MAX];
};

// ---- Context Header (修改点 #7: 固定 ABI) ----
struct agent_context_header {
  uint32 magic;              // 魔数 AGENT_CONTEXT_HEADER_MAGIC
  uint32 version;            // ABI 版本
  uint32 header_size;        // header 大小
  uint32 node_size;          // 节点大小
  uint64 region_size;
  uint64 path_offset;
  uint64 path_length;
  uint64 node_count;
  uint64 dropped_nodes;
  uint64 generation;         // 一致性版本号 (写前递增)
  uint64 first_sequence;     // 首个节点序列号
  uint64 next_sequence;      // 下一个序列号
  uint64 last_result_len;
  uint64 last_timestamp;
  char last_tool[AGENT_TOOL_NAME_MAX];
  char last_result[AGENT_TOOL_RESULT_MAX];
};

// ---- 内核可信 Context 摘要 (修改点 #6) ----
struct agent_context_digest {
  uint64 sequence;
  uint64 request_id;
  uint64 request_hash;
  uint64 result_hash;
  uint64 previous_hash;
  uint64 current_hash;
  int status;
};

// ---- 等待事件结构 (修改点 #17: 增加 timeout/cancel) ----
struct agent_wait_event {
  int reason;               // AGENT_WAIT_MESSAGE/FILEMOD/HEARTBEAT/TIMEOUT/CANCELLED/NO_SOURCE
  uint32 reserved;
  uint64 tick;
  uint64 sequence;          // 修改点 #1: 消息序列号
  uint64 request_id;        // 修改点 #4: 请求 ID
  int from_pid;             // 修改点 #1: 发送者 PID
  int msg_type;             // 修改点 #1: 消息类型
  int msg_length;           // 修改点 #1: 消息长度
  char message[AGENT_MESSAGE_MAX];
  char file[AGENT_MESSAGE_MAX];
};

// ---- FIFO 消息结构 (修改点 #1) ----
struct agent_message {
  int from_pid;
  int type;                 // AGENT_MESSAGE_TYPE_NORMAL/SYSTEM
  int length;
  uint64 sequence;
  uint64 request_id;
  char payload[AGENT_MESSAGE_MAX];
};

struct agent_mailbox {
  struct agent_message queue[AGENT_MAILBOX_CAP];
  int head;
  int tail;
  int count;
  int normal_count;
  int system_count;
  uint64 dropped;
  uint64 system_dropped;
  uint64 next_sequence;
};

// ---- 文件编辑租约 (修改点 #2) ----
struct agent_edit_lease {
  int used;
  uint dev;
  uint inum;
  int owner_pid;
  uint64 owner_generation;
  uint64 lease_id;
  uint64 base_version;
  uint64 expiry_tick;
};

// ---- agent_wait 超时定时器 (修改点 #17) ----
struct agent_wait_timer {
  int pid;
  uint64 identity_generation;
  uint64 wait_generation;
  uint64 deadline;
};

// ---- 动态工具请求 ----
struct agent_dynamic_tool_request {
  int request_id;
  int caller_pid;
  uint64 span_id;           // 修改点 #19
  char tool[AGENT_TOOL_NAME_MAX];
  char params[AGENT_TOOL_PARAM_MAX];
};

// ---- 统一日志记录 (修改点 #19) ----
struct agent_trace_record {
  uint64 tick;
  uint64 span_id;
  uint64 request_id;
  int agent_pid;
  int agent_role;
  char action[32];
  int status;
  char cause[64];
};

// ---- 函数声明 ----
void agent_init_proc(struct proc *p);
void agent_global_init(void);
void agent_after_fork(struct proc *dst, struct proc *src);
void agent_context_clear(struct proc *p);
int agent_sync_header(struct proc *p);
uint64 agent_mark_current(int type, int heartbeat_interval, uint64 resource_quota);
int agent_get_info(struct proc *p, struct agent_info *info);
int agent_context_push_node(struct proc *p, struct agent_context_node *node);
int agent_context_query(struct proc *p, uint64 dst, uint64 len);
int agent_context_rollback(struct proc *p, uint64 keep_nodes);
int agent_copy_tool_list(struct proc *p, uint64 dst, uint64 len);
int agent_tool_call(struct proc *p, struct agent_tool_request *req,
                    struct agent_tool_response *resp);
int agent_tool_call_batch(struct proc *p, struct agent_tool_batch_request *breq,
                           struct agent_tool_batch_response *bresp);
int agent_tool_schema_get(struct proc *p, const char *name,
                           struct agent_tool_schema *schema);
int agent_tool_schema_list(struct proc *p, uint64 dst, uint64 len);
int agent_proc_heartbeat_set(struct proc *p, int interval);
int agent_proc_heartbeat_stop(struct proc *p);
int agent_proc_watch(struct proc *p, int mask);
int agent_proc_unwatch(struct proc *p, int mask);
int agent_proc_watch_file(struct proc *p, uint64 upath);
int agent_proc_sched_set(struct proc *p, int priority, int quota);
int agent_proc_wait(struct proc *p, int continue_loop, uint64 uevent,
                     int timeout_ticks);
int agent_proc_priority_set(struct proc *p, int priority);
int agent_proc_cap_set(struct proc *p, uint64 caps);
int agent_proc_role_set(struct proc *p, int role);
int agent_tool_register(struct proc *p, const char *name, int flags);
int agent_tool_recv(struct proc *p, struct agent_dynamic_tool_request *out);
int agent_tool_reply(struct proc *p, int request_id, const char *result,
                     int status);
int agent_proc_lease_begin(struct proc *p, const char *path, uint64 *lease_id,
                            uint64 *base_version);
int agent_proc_lease_commit(struct proc *p, uint64 lease_id, uint64 expected_version);
int agent_proc_lease_abort(struct proc *p, uint64 lease_id);
void agent_proc_exit(struct proc *p);
void agentfs_tool_set_file_attr(struct agent_tool_request *req,
                                struct agent_tool_response *resp);
void agentfs_tool_get_file_attr(struct agent_tool_request *req,
                                struct agent_tool_response *resp);
void agentfs_tool_del_file_attr(struct agent_tool_request *req,
                                struct agent_tool_response *resp);
void agentfs_tool_query_file(struct proc *p, struct agent_tool_request *req,
                             struct agent_tool_response *resp);
void agentfs_content_changed(void);
void agent_signal_filemod(void);
void agent_signal_event_locked(struct proc *p, int event, uint64 now);
int agent_schedule_score(struct proc *p, uint64 now);
void agent_tick(uint64 now);
void agent_notify_file_modified(uint dev, uint inum);
int agent_send_message(struct proc *src, int target_pid, int msg_type,
                        const char *payload, uint64 request_id);
int agent_proc_query_agent(struct proc *p, int role, int capability, int group,
                            uint64 dst, uint64 len);
void agent_trace(struct proc *p, const char *action, int status,
                  const char *cause);
void agent_trace_span(struct proc *p, uint64 span_id, uint64 request_id,
                       const char *action, int status, const char *cause);
void agent_audit_record(struct proc *p, uint64 target, const char *action,
                         int decision, int status, const char *cause);
int agent_context_digest_verify(struct proc *p);
void agent_lease_reap_expired(uint64 now);
void agent_lease_reap_pid(int pid);

#endif
