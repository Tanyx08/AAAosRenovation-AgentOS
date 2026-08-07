#!/usr/bin/env sh
# Verify that plain xv6 and AgentOS CodeLab produced the same business result.

set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 baseline_plainlab.log agentos_planner.log" >&2
  exit 2
fi

plain_log="$1"
agentos_log="$2"

result_line()
{
  target="$1"
  log="$2"
  awk -v wanted="$target" '
    /^\[RESULT\] / {
      matched = 0
      for(i = 1; i <= NF; i++)
        if($i == "target=" wanted)
          matched = 1
      if(matched)
        line = $0
    }
    END { print line }
  ' "$log"
}

field()
{
  line="$1"
  key="$2"
  awk -v input="$line" -v wanted="$key" '
    BEGIN {
      count = split(input, fields, " ")
      for(i = 1; i <= count; i++){
        prefix = wanted "="
        if(index(fields[i], prefix) == 1){
          print substr(fields[i], length(prefix) + 1)
          exit
        }
      }
    }
  '
}

plain_line="$(result_line plain "$plain_log")"
agentos_line="$(result_line agentos "$agentos_log")"

if [ -z "$plain_line" ] || [ -z "$agentos_line" ]; then
  echo "[VERIFY] suite=dual business_equivalent=0 status=FAIL cause=missing_result"
  exit 1
fi

plain_found="$(field "$plain_line" found)"
plain_patch="$(field "$plain_line" patch_ok)"
plain_test="$(field "$plain_line" test_ok)"
plain_review="$(field "$plain_line" review_ok)"
plain_initial_size="$(field "$plain_line" initial_size)"
plain_initial_hash="$(field "$plain_line" initial_hash)"
plain_size="$(field "$plain_line" file_size)"
plain_hash="$(field "$plain_line" file_hash)"
plain_status="$(field "$plain_line" status)"

agentos_found="$(field "$agentos_line" found)"
agentos_patch="$(field "$agentos_line" patch_ok)"
agentos_test="$(field "$agentos_line" test_ok)"
agentos_review="$(field "$agentos_line" review_ok)"
agentos_initial_size="$(field "$agentos_line" initial_size)"
agentos_initial_hash="$(field "$agentos_line" initial_hash)"
agentos_size="$(field "$agentos_line" file_size)"
agentos_hash="$(field "$agentos_line" file_hash)"
agentos_status="$(field "$agentos_line" status)"

equivalent=1
for value in "$plain_found" "$plain_patch" "$plain_test" "$plain_review"              "$agentos_found" "$agentos_patch" "$agentos_test" "$agentos_review"; do
  if [ "$value" != "1" ]; then
    equivalent=0
  fi
done

if [ "$plain_status" != "PASS" ] || [ "$agentos_status" != "PASS" ] ||
   [ "$plain_initial_size" != "$agentos_initial_size" ] ||
   [ "$plain_initial_hash" != "$agentos_initial_hash" ] ||
   [ "$plain_size" != "$agentos_size" ] ||
   [ "$plain_hash" != "$agentos_hash" ]; then
  equivalent=0
fi

echo "[VERIFY] suite=dual initial_plain=$plain_initial_hash initial_agentos=$agentos_initial_hash initial_equal=$([ "$plain_initial_hash" = "$agentos_initial_hash" ] && echo 1 || echo 0)"
echo "[VERIFY] suite=dual final_plain=$plain_hash final_agentos=$agentos_hash size_plain=$plain_size size_agentos=$agentos_size final_equal=$([ "$plain_hash" = "$agentos_hash" ] && [ "$plain_size" = "$agentos_size" ] && echo 1 || echo 0)"
echo "[VERIFY] suite=dual patch_equal=$([ "$plain_patch" = "$agentos_patch" ] && echo 1 || echo 0) test_equal=$([ "$plain_test" = "$agentos_test" ] && echo 1 || echo 0) review_equal=$([ "$plain_review" = "$agentos_review" ] && echo 1 || echo 0) business_equivalent=$equivalent status=$([ "$equivalent" -eq 1 ] && echo PASS || echo FAIL)"

if [ "$equivalent" -ne 1 ]; then
  exit 1
fi
