#ifndef XV6_LLM_BRIDGE_H
#define XV6_LLM_BRIDGE_H

#include "kernel/types.h"

#define LLM_ROLE_MAX 32
#define LLM_PROMPT_MAX 256
#define LLM_ACTION_MAX 128
#define LLM_RESPONSE_MAX 256

enum llm_request_state {
  LLM_REQ_UNUSED = 0,
  LLM_REQ_PENDING = 1,
  LLM_REQ_RUNNING = 2,
  LLM_REQ_DONE = 3,
  LLM_REQ_CANCELED = 4,
  LLM_REQ_FAILED = 5,
};

struct agent_action {
  char tool[LLM_ROLE_MAX];
  char params[LLM_ACTION_MAX];
};

struct llm_request {
  int request_id;
  int owner_pid;
  int state;
  char role[LLM_ROLE_MAX];
  char prompt[LLM_PROMPT_MAX];
};

struct llm_response {
  int request_id;
  int state;
  int action_count;
  char text[LLM_RESPONSE_MAX];
  struct agent_action action;
};

int llm_request_submit(struct llm_request *req);
int llm_wait(int request_id, struct llm_response *resp);
int llm_poll(int request_id, struct llm_response *resp);
int llm_cancel(int request_id);

#endif
