// System call numbers
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_kill    6
#define SYS_exec    7
#define SYS_fstat   8
#define SYS_chdir   9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_sleep  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
#define SYS_mknod  17
#define SYS_unlink 18
#define SYS_link   19
#define SYS_mkdir  20
#define SYS_close  21
#define SYS_mmap   22
#define SYS_munmap 23
#define SYS_agent_create 24
#define SYS_agent_info 25
#define SYS_tool_call 26
#define SYS_tool_list 27
#define SYS_context_push 28
#define SYS_context_query 29
#define SYS_context_rollback 30
#define SYS_context_clear 31
#define SYS_agent_heartbeat_set 32
#define SYS_agent_heartbeat_stop 33
#define SYS_agent_watch 34
#define SYS_agent_wait 35
#define SYS_agent_unwatch 36
#define SYS_agent_priority_set 37
#define SYS_tool_register 38
#define SYS_tool_recv 39
#define SYS_tool_reply 40
#define SYS_agent_watch_file 41
#define SYS_agent_sched_set 42
// 决赛新增 syscall (修改点 #1-#24)
#define SYS_agent_wait_timeout 43    // 修改点 #17: agent_wait 带 timeout
#define SYS_tool_call_batch 44      // 修改点 #22: 批量工具调用
#define SYS_tool_schema 45          // 修改点 #14: 工具 schema 查询
#define SYS_agent_cap_set 46        // 修改点 #3: 设置 capability
#define SYS_agent_lease_begin 47    // 修改点 #2: 文件编辑租约 begin
#define SYS_agent_lease_commit 48   // 修改点 #2: 文件编辑租约 commit
#define SYS_agent_lease_abort 49    // 修改点 #2: 文件编辑租约 abort
#define SYS_agent_query_agent 50    // 修改点 #4/#2: Agent 发现
#define SYS_agent_role_set 51       // 修改点 #3: 设置 Agent 角色
#define SYS_agent_context_verify 52 // 校验内核可信 Context 摘要链
