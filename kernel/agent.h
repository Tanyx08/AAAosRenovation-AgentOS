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

#define AGENT_TOOL_NAME_MAX (32)
#define AGENT_TOOL_PARAM_MAX (128)
#define AGENT_TOOL_RESULT_MAX (512)
#define AGENT_CONTEXT_REQ_MAX (96)
#define AGENT_CONTEXT_RES_MAX (160)
#define AGENT_CONTEXT_MAX_NODES (16)
#define AGENT_MESSAGE_MAX (128)

#define AGENT_TOOL_OK (0)
#define AGENT_TOOL_ERR_TOOL_NOT_FOUND (-1)
#define AGENT_TOOL_ERR_BAD_PARAM (-2)
#define AGENT_TOOL_ERR_NOT_AGENT (-3)
#define AGENT_TOOL_ERR_NO_SPACE (-4)

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

#endif
