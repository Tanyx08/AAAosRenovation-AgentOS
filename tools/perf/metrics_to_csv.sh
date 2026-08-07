#!/usr/bin/env sh
# Convert AgentOS [METRIC] log lines into a wide CSV table.
#
# Usage:
#   tools/perf/metrics_to_csv.sh raw.log > metrics.csv
#   cat raw.log | tools/perf/metrics_to_csv.sh > metrics.csv

awk '
BEGIN {
  header = "suite,case,target,mode,event,phase,agents,senders,messages,consumer,capacity,normal_capacity,files,run,delay,ticks,scanned,plan_scanned,full_scanned,matches,total_ticks,files_scanned,tool_calls,query_file_calls,syscalls,polling_loops,duplicate_queries,context_hit,cache_hit,cache_hits,cache_misses,wait_calls,wait_ticks,messages_received,dispatch,p50_wait,p95_wait,max_wait,fairness,high_dispatch,low_dispatch,accepted,received,busy,order_errors,duplicates,wrong_sender,wakeup_latency,trigger_tick,handle_tick,cpu_ticks,status"
  n = split(header, cols, ",")
  print header
}

function csv_escape(value) {
  gsub(/"/, "\"\"", value)
  if(value ~ /[",\r\n]/)
    return "\"" value "\""
  return value
}

/^\[METRIC\]/ {
  delete kv
  for(i = 2; i <= NF; i++) {
    eq = index($i, "=")
    if(eq <= 1)
      continue
    key = substr($i, 1, eq - 1)
    val = substr($i, eq + 1)
    kv[key] = val
  }

  line = ""
  for(i = 1; i <= n; i++) {
    value = (cols[i] in kv) ? kv[cols[i]] : ""
    if(i > 1)
      line = line ","
    line = line csv_escape(value)
  }
  print line
}
' "$@"
