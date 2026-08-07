#!/usr/bin/env bash
# Run AgentOS quantitative benchmarks in QEMU and save raw/CSV logs.
#
# Usage:
#   tools/perf/run_agentos_perf.sh [quick|small|full]
#
# Environment:
#   PERF_TIMEOUT=seconds      Timeout for each benchmark command.

set -euo pipefail

MODE="${1:-quick}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_ID="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$ROOT_DIR/dual-results/run_$RUN_ID"
RAW_DIR="$OUT_DIR/raw"
CSV_DIR="$OUT_DIR/csv"
RAW_LOG="$RAW_DIR/agentos_perf.log"
CSV_FILE="$CSV_DIR/agentos_metrics.csv"

case "$MODE" in
  quick)
    COMMANDS=(
      "agentfsmetric"
      "contextmetric"
      "waitmetric"
      "schedmetric"
      "mailbench"
    )
    DEFAULT_TIMEOUT=300
    ;;
  small)
    COMMANDS=(
      "agentfsmetric small"
      "contextmetric"
      "waitmetric small"
      "schedmetric small"
      "mailbench small"
    )
    DEFAULT_TIMEOUT=600
    ;;
  full)
    COMMANDS=(
      "agentfsmetric full"
      "contextmetric"
      "waitmetric full"
      "schedmetric full"
      "mailbench full"
    )
    DEFAULT_TIMEOUT=1800
    ;;
  *)
    echo "usage: $0 [quick|small|full]" >&2
    exit 2
    ;;
esac

TIMEOUT="${PERF_TIMEOUT:-$DEFAULT_TIMEOUT}"

mkdir -p "$RAW_DIR" "$CSV_DIR"

echo "[HOST] run_id=$RUN_ID mode=$MODE"
echo "[HOST] running isolated AgentOS benchmarks, raw log: $RAW_LOG"
: > "$RAW_LOG"
for cmd in "${COMMANDS[@]}"; do
  suite="${cmd%% *}"
  suite_log="$RAW_DIR/$suite.log"
  suite_image="$RAW_DIR/$suite.img"
  echo "[HOST] rebuilding clean image for command: $cmd"
  (cd "$ROOT_DIR" && make -B kernel/kernel fs.img >/dev/null)
  python3 "$SCRIPT_DIR/qemu_command_driver.py" \
    --command "$cmd" --timeout "$TIMEOUT" --disk-image "$suite_image" \
    > "$suite_log" 2>&1
  if grep -Eq 'status=FAIL|\[SUMMARY\].*fail=[1-9][0-9]*' "$suite_log"; then
    echo "[HOST] benchmark failed: $cmd" >&2
    exit 1
  fi
  if ! grep -q '^\[SUMMARY\]' "$suite_log"; then
    echo "[HOST] benchmark did not produce a summary: $cmd" >&2
    exit 1
  fi
  cat "$suite_log" >> "$RAW_LOG"
  printf '\n' >> "$RAW_LOG"
done

echo "[HOST] converting metrics to CSV: $CSV_FILE"
"$SCRIPT_DIR/metrics_to_csv.sh" "$RAW_LOG" > "$CSV_FILE"

echo "[HOST] done"
echo "[HOST] raw=$RAW_LOG"
echo "[HOST] csv=$CSV_FILE"
