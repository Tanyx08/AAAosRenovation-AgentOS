#!/usr/bin/env bash
# Run the plain xv6 and AgentOS CodeLab paths and export comparable metrics.
#
# Usage:
#   tools/perf/run_dual_codelab.sh
#
# Environment:
#   PERF_RUN_SECONDS=seconds  Time to wait after sending each QEMU command.
#   PERF_TIMEOUT=seconds      Outer timeout guard for each QEMU run.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BASELINE_DIR="$ROOT_DIR/baseline-xv6"
RUN_ID="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$ROOT_DIR/dual-results/run_$RUN_ID"
RAW_DIR="$OUT_DIR/raw"
CSV_DIR="$OUT_DIR/csv"
BASELINE_LOG="$RAW_DIR/baseline_plainlab.log"
AGENTOS_LOG="$RAW_DIR/agentos_planner.log"
CSV_FILE="$CSV_DIR/dual_codelab.csv"
RUN_SECONDS="${PERF_RUN_SECONDS:-180}"
TIMEOUT="${PERF_TIMEOUT:-$((RUN_SECONDS + 60))}"

mkdir -p "$RAW_DIR" "$CSV_DIR"

run_qemu_command() {
  local dir="$1"
  local command="$2"
  local log="$3"

  {
    sleep 2
    printf "%s\n" "$command"
    sleep "$RUN_SECONDS"
    printf "\001x"
  } | (cd "$dir" && timeout "$TIMEOUT" make qemu) > "$log" 2>&1 || true
}

echo "[HOST] run_id=$RUN_ID"
echo "[HOST] building baseline-xv6 fs.img"
(cd "$BASELINE_DIR" && make fs.img)
echo "[HOST] running plainlab, raw log: $BASELINE_LOG"
run_qemu_command "$BASELINE_DIR" "plainlab" "$BASELINE_LOG"

echo "[HOST] building AgentOS fs.img"
(cd "$ROOT_DIR" && make fs.img)
echo "[HOST] running planner_agent, raw log: $AGENTOS_LOG"
run_qemu_command "$ROOT_DIR" "planner_agent" "$AGENTOS_LOG"

echo "[HOST] converting dual metrics to CSV: $CSV_FILE"
"$SCRIPT_DIR/metrics_to_csv.sh" "$BASELINE_LOG" "$AGENTOS_LOG" > "$CSV_FILE"

echo "[HOST] done"
echo "[HOST] baseline_raw=$BASELINE_LOG"
echo "[HOST] agentos_raw=$AGENTOS_LOG"
echo "[HOST] csv=$CSV_FILE"
