#include "kernel/types.h"
#include "kernel/agent.h"
#include "user/user.h"

#define MAX_SENDERS 4
#define MAX_MESSAGES 30

struct sender_result {
  int sid;
  int pid;
  int sent;
  int accepted;
  int busy;
  int errors;
};

static int failures;

static void
append_str(char *dst, int *pos, const char *src, int max)
{
  while(*src && *pos < max - 1)
    dst[(*pos)++] = *src++;
  dst[*pos] = 0;
}

static void
append_uint(char *dst, int *pos, uint64 value, int max)
{
  char tmp[24];
  int n = 0;

  if(value == 0){
    append_str(dst, pos, "0", max);
    return;
  }
  while(value > 0 && n < (int)sizeof(tmp)){
    tmp[n++] = '0' + value % 10;
    value /= 10;
  }
  while(n > 0 && *pos < max - 1)
    dst[(*pos)++] = tmp[--n];
  dst[*pos] = 0;
}

static int
parse_field(const char *s, const char *field)
{
  int flen = strlen(field);
  int i;

  for(; *s; s++){
    for(i = 0; i < flen && s[i] == field[i]; i++)
      ;
    if(i == flen && s[flen] == '='){
      int value = 0;

      s += flen + 1;
      while(*s >= '0' && *s <= '9'){
        value = value * 10 + *s - '0';
        s++;
      }
      return value;
    }
  }
  return -1;
}

static int
call_tool(const char *tool, const char *params, struct agent_tool_response *resp)
{
  struct agent_tool_request req;

  memset(&req, 0, sizeof(req));
  strcpy(req.tool, tool);
  strcpy(req.params, params);
  return tool_call(&req, resp);
}

static int
send_message_to(int pid, const char *message)
{
  struct agent_tool_response resp;
  char params[AGENT_TOOL_PARAM_MAX];
  int pos = 0;

  memset(params, 0, sizeof(params));
  append_str(params, &pos, "target_pid=", sizeof(params));
  append_uint(params, &pos, pid, sizeof(params));
  append_str(params, &pos, ";message=", sizeof(params));
  append_str(params, &pos, message, sizeof(params));
  return call_tool("send_message", params, &resp);
}

static void
sender_proc(int sid, int target_pid, int messages, int result_fd)
{
  struct sender_result result;
  char msg[AGENT_MESSAGE_MAX];
  int ret;
  int pos;

  memset(&result, 0, sizeof(result));
  agent_create(AGENT_TYPE_WORKER, 0, 256);
  result.sid = sid;
  result.pid = getpid();

  for(int seq = 0; seq < messages; seq++){
    memset(msg, 0, sizeof(msg));
    pos = 0;
    append_str(msg, &pos, "sid=", sizeof(msg));
    append_uint(msg, &pos, sid, sizeof(msg));
    append_str(msg, &pos, ";seq=", sizeof(msg));
    append_uint(msg, &pos, seq, sizeof(msg));
    append_str(msg, &pos, ";pid=", sizeof(msg));
    append_uint(msg, &pos, result.pid, sizeof(msg));

    result.sent++;
    ret = send_message_to(target_pid, msg);
    if(ret == AGENT_TOOL_OK)
      result.accepted++;
    else if(ret == AGENT_TOOL_ERR_BUSY)
      result.busy++;
    else
      result.errors++;
  }

  write(result_fd, &result, sizeof(result));
  exit(result.errors == 0 ? 0 : 1);
}

static void
read_exact(int fd, char *buf, int n)
{
  int got = 0;
  int r;

  while(got < n){
    r = read(fd, buf + got, n - got);
    if(r <= 0)
      break;
    got += r;
  }
}

static void
consume_until_idle(int sender_count, int messages, int *received,
                   int *order_errors, int *duplicates, int *wrong_sender)
{
  struct agent_wait_event event;
  int last_seq[MAX_SENDERS];
  int seen[MAX_SENDERS][MAX_MESSAGES];
  int idle_rounds = 0;

  memset(last_seq, 0xff, sizeof(last_seq));
  memset(seen, 0, sizeof(seen));

  while(idle_rounds < 8){
    int reason;
    int sid;
    int seq;
    int pid;

    memset(&event, 0, sizeof(event));
    reason = agent_wait_timeout(1, 4, &event);
    if(reason == AGENT_WAIT_TIMEOUT || reason == AGENT_WAIT_NO_SOURCE){
      idle_rounds++;
      continue;
    }
    if(reason != AGENT_WAIT_MESSAGE){
      (*wrong_sender)++;
      continue;
    }

    idle_rounds = 0;
    (*received)++;
    sid = parse_field(event.message, "sid");
    seq = parse_field(event.message, "seq");
    pid = parse_field(event.message, "pid");
    if(sid < 0 || sid >= sender_count || seq < 0 || seq >= messages ||
       pid != event.from_pid){
      (*wrong_sender)++;
      continue;
    }
    if(last_seq[sid] >= 0 && seq <= last_seq[sid])
      (*order_errors)++;
    last_seq[sid] = seq;
    if(seen[sid][seq])
      (*duplicates)++;
    seen[sid][seq] = 1;
  }
}

static void
run_mail_case(int senders, int messages, int slow_consumer)
{
  struct sender_result results[MAX_SENDERS];
  int result_pipe[2];
  int pids[MAX_SENDERS];
  int sent = 0;
  int accepted = 0;
  int busy = 0;
  int errors = 0;
  int received = 0;
  int order_errors = 0;
  int duplicates = 0;
  int wrong_sender = 0;
  int status = -1;
  int target_pid = getpid();
  int ok;

  memset(results, 0, sizeof(results));
  agent_watch(AGENT_WATCH_MESSAGE);
  agent_heartbeat_stop();
  if(pipe(result_pipe) < 0){
    printf("[METRIC] suite=mailbox senders=%d messages=%d consumer=%s capacity=%d sent=0 accepted=0 received=0 busy=0 order_errors=0 duplicates=0 wrong_sender=0 status=FAIL\n",
           senders, messages, slow_consumer ? "slow" : "fast",
           AGENT_MAILBOX_CAP);
    failures++;
    return;
  }

  for(int i = 0; i < senders; i++){
    pids[i] = fork();
    if(pids[i] == 0){
      close(result_pipe[0]);
      sender_proc(i, target_pid, messages, result_pipe[1]);
    }
  }
  close(result_pipe[1]);

  if(slow_consumer)
    sleep(8);
  consume_until_idle(senders, messages, &received, &order_errors,
                     &duplicates, &wrong_sender);

  for(int i = 0; i < senders; i++){
    read_exact(result_pipe[0], (char *)&results[i], sizeof(results[i]));
    sent += results[i].sent;
    accepted += results[i].accepted;
    busy += results[i].busy;
    errors += results[i].errors;
  }
  close(result_pipe[0]);

  for(int i = 0; i < senders; i++)
    wait(&status);

  /* Drain any messages that arrived while sender summaries were read. */
  consume_until_idle(senders, messages, &received, &order_errors,
                     &duplicates, &wrong_sender);
  agent_unwatch(AGENT_WATCH_MESSAGE);

  ok = sent == senders * messages &&
       sent == accepted + busy + errors &&
       received == accepted &&
       errors == 0 &&
       order_errors == 0 &&
       duplicates == 0 &&
       wrong_sender == 0 &&
       accepted > 0;

  printf("[METRIC] suite=mailbox senders=%d messages=%d consumer=%s capacity=%d normal_capacity=%d sent=%d accepted=%d received=%d busy=%d order_errors=%d duplicates=%d wrong_sender=%d status=%s\n",
         senders, messages, slow_consumer ? "slow" : "fast",
         AGENT_MAILBOX_CAP,
         AGENT_MAILBOX_CAP - AGENT_MAILBOX_SYSTEM_SLOTS,
         sent, accepted, received, busy, order_errors, duplicates,
         wrong_sender, ok ? "PASS" : "FAIL");
  if(!ok)
    failures++;
}

int
main(int argc, char *argv[])
{
  int full = 0;
  int small = 0;

  if(argc > 1 && strcmp(argv[1], "full") == 0)
    full = 1;
  else if(argc > 1 && strcmp(argv[1], "small") == 0)
    small = 1;

  if((uint64)agent_create(AGENT_TYPE_PRIMARY, 0, 512) == 0){
    printf("[SUMMARY] suite=agentmailbench pass=0 fail=1\n");
    exit(1);
  }

  printf("[TEST] suite=agentmailbench case=setup mode=%s status=BEGIN\n",
         full ? "full" : (small ? "small" : "quick"));
  if(!small && !full){
    run_mail_case(1, 10, 0);
  } else if(small){
    run_mail_case(1, 10, 0);
    run_mail_case(2, 10, 1);
    run_mail_case(2, 30, 1);
  } else {
    run_mail_case(1, 10, 0);
    run_mail_case(2, 10, 0);
    run_mail_case(2, 30, 1);
    run_mail_case(4, 10, 0);
    run_mail_case(4, 30, 1);
  }

  printf("[SUMMARY] suite=agentmailbench pass=%d fail=%d\n",
         failures == 0 ? 1 : 0, failures);
  exit(failures ? 1 : 0);
}
