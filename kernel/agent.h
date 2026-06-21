// Agent-OS 公共内核 ABI。
//
// 这个头文件定义了 Agent-OS 内核子系统与用户态 Agent 程序共享使用的
// 常量、结构体和函数声明。内容包括 Agent Context 布局、工具调用请求/
// 响应格式、等待事件结构、动态工具请求结构，以及 Agent 管理相关的
// 主要内核入口函数。

#ifndef XV6_AGENT_H
#define XV6_AGENT_H

#include "types.h"

#define AGENT_CONTEXT_REGION_SIZE (8192)
#define AGENT_CONTEXT_HEADER_MAGIC (0x41474e54U)
#define AGENT_CONTEXT_HEADER_VERSION (1U)

#define AGENT_TYPE_NORMAL (0)
#define AGENT_TYPE_PRIMARY (1)
#define AGENT_TYPE_WORKER (2)

#define AGENT_LOOP_IDLE (0)
#define AGENT_LOOP_READY (1)
#define AGENT_LOOP_RUNNING (2)
#define AGENT_LOOP_WAITING (3)
#define AGENT_LOOP_ROLLED_BACK (4)
#define AGENT_LOOP_DONE (5)

#define AGENT_EVENT_NONE (0)
#define AGENT_EVENT_HEARTBEAT (1)
#define AGENT_EVENT_MESSAGE (2)
#define AGENT_EVENT_FILEMOD (4)

#define AGENT_WATCH_MESSAGE AGENT_EVENT_MESSAGE
#define AGENT_WATCH_FILEMOD AGENT_EVENT_FILEMOD

#define AGENT_TOOL_FLAG_PUBLIC (1)

#define AGENT_TOOL_NAME_MAX (32)
#define AGENT_TOOL_PARAM_MAX (128)
#define AGENT_TOOL_RESULT_MAX (512)
#define AGENT_CONTEXT_REQ_MAX (96)
#define AGENT_CONTEXT_RES_MAX (160)
#define AGENT_CONTEXT_MAX_NODES (16)
#define AGENT_MESSAGE_MAX (128)
#define AGENT_DYNAMIC_TOOL_MAX (8)
#define AGENT_DYNAMIC_REQUEST_MAX (8)

#define AGENT_TOOL_OK (0)
#define AGENT_TOOL_ERR_TOOL_NOT_FOUND (-1)
#define AGENT_TOOL_ERR_BAD_PARAM (-2)
#define AGENT_TOOL_ERR_NOT_AGENT (-3)
#define AGENT_TOOL_ERR_NO_SPACE (-4)
#define AGENT_TOOL_ERR_BUSY (-5)
#define AGENT_TOOL_ERR_PERMISSION (-6)
#define AGENT_TOOL_ERR_SERVICE_GONE (-7)

#define AGENT_SCHED_PRIORITY_MIN (1)
#define AGENT_SCHED_PRIORITY_MAX (8)
#define AGENT_SCHED_QUOTA_MIN (1)
#define AGENT_SCHED_QUOTA_MAX (8)

struct proc;

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
};

struct agent_tool_request {
  char tool[AGENT_TOOL_NAME_MAX];
  char params[AGENT_TOOL_PARAM_MAX];
};

struct agent_tool_response {
  int status;
  uint32 result_len;
  char result[AGENT_TOOL_RESULT_MAX];
};

struct agent_context_node {
  uint64 timestamp_ms;
  char request[AGENT_CONTEXT_REQ_MAX];
  char result[AGENT_CONTEXT_RES_MAX];
};

struct agent_context_header {
  uint32 magic;
  uint32 version;
  uint64 region_size;
  uint64 path_offset;
  uint64 path_length;
  uint64 node_count;
  uint64 dropped_nodes;
  uint64 last_result_len;
  uint64 last_timestamp;
  char last_tool[AGENT_TOOL_NAME_MAX];
  char last_result[AGENT_TOOL_RESULT_MAX];
};

struct agent_wait_event {
  int reason;
  uint32 reserved;
  uint64 tick;
  char message[AGENT_MESSAGE_MAX];
  char file[AGENT_MESSAGE_MAX];
};

struct agent_dynamic_tool_request {
  int request_id;
  int caller_pid;
  char tool[AGENT_TOOL_NAME_MAX];
  char params[AGENT_TOOL_PARAM_MAX];
};

void agent_init_proc(struct proc *p);
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
int agent_proc_heartbeat_set(struct proc *p, int interval);
int agent_proc_heartbeat_stop(struct proc *p);
int agent_proc_watch(struct proc *p, int mask);
int agent_proc_unwatch(struct proc *p, int mask);
int agent_proc_watch_file(struct proc *p, uint64 upath);
int agent_proc_sched_set(struct proc *p, int priority, int quota);
int agent_proc_wait(struct proc *p, int continue_loop, uint64 uevent);
int agent_proc_priority_set(struct proc *p, int priority);
int agent_tool_register(struct proc *p, const char *name, int flags);
int agent_tool_recv(struct proc *p, struct agent_dynamic_tool_request *out);
int agent_tool_reply(struct proc *p, int request_id, const char *result,
                     int status);
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

#endif
