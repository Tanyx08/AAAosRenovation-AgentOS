#!/usr/bin/env bash
# Run AgentOS quantitative benchmarks in QEMU and save raw/CSV logs.
#
# Usage:
#   tools/perf/run_agentos_perf.sh [quick|small|full]
#
# Environment:
#   PERF_RUN_SECONDS=seconds  Time to wait after sending benchmark commands.
#   PERF_TIMEOUT=seconds      Outer timeout guard.

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
    DEFAULT_RUN_SECONDS=120
    ;;
  small)
    COMMANDS=(
      "agentfsmetric small"
      "contextmetric"
      "waitmetric small"
      "schedmetric small"
      "mailbench small"
    )
    DEFAULT_RUN_SECONDS=600
    ;;
  full)
    COMMANDS=(
      "agentfsmetric full"
      "contextmetric"
      "waitmetric full"
      "schedmetric full"
      "mailbench full"
    )
    DEFAULT_RUN_SECONDS=1800
    ;;
  *)
    echo "usage: $0 [quick|small|full]" >&2
    exit 2
    ;;
esac

RUN_SECONDS="${PERF_RUN_SECONDS:-$DEFAULT_RUN_SECONDS}"
TIMEOUT="${PERF_TIMEOUT:-$((RUN_SECONDS + 60))}"

mkdir -p "$RAW_DIR" "$CSV_DIR"

echo "[HOST] run_id=$RUN_ID mode=$MODE"
echo "[HOST] building AgentOS fs.img"
(cd "$ROOT_DIR" && make fs.img)

echo "[HOST] running AgentOS benchmarks, raw log: $RAW_LOG"
{
  sleep 2
  for cmd in "${COMMANDS[@]}"; do
    printf "%s\n" "$cmd"
    sleep 1
  done
  # Leave enough time for commands. timeout(1) is still the outer guard.
  sleep "$RUN_SECONDS"
  printf "\001x"
} | (cd "$ROOT_DIR" && timeout "$TIMEOUT" make qemu) > "$RAW_LOG" 2>&1 || true

echo "[HOST] converting metrics to CSV: $CSV_FILE"
"$SCRIPT_DIR/metrics_to_csv.sh" "$RAW_LOG" > "$CSV_FILE"

echo "[HOST] done"
echo "[HOST] raw=$RAW_LOG"
echo "[HOST] csv=$CSV_FILE"
