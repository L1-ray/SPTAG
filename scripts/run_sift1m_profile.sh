#!/usr/bin/env bash
set -euo pipefail

BASE=/home/ray/code/SPTAG
BIN="$BASE/Release/ssdserving"
CFG_DIR="$BASE/configs"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <a3|a4> [cache_mode]"
  echo "  cache_mode: cold|hot (default: cold)"
  exit 1
fi

PROFILE="$1"
CACHE_MODE="${2:-cold}"

case "$PROFILE" in
  a3)
    CFG="$CFG_DIR/sift1m_ir64_a3_ref_rollback.ini"
    ;;
  a4)
    CFG="$CFG_DIR/sift1m_ir64_a4_cons_v3.ini"
    ;;
  *)
    echo "Invalid profile: $PROFILE (expected a3 or a4)"
    exit 1
    ;;
esac

if [[ ! -f "$CFG" ]]; then
  echo "Missing config: $CFG"
  exit 1
fi

if [[ "$CACHE_MODE" == "cold" ]]; then
  sync
  echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
elif [[ "$CACHE_MODE" != "hot" ]]; then
  echo "Invalid cache_mode: $CACHE_MODE (expected cold or hot)"
  exit 1
fi

LOG_DIR="$BASE/results/adaptive_budget/profile_runs"
mkdir -p "$LOG_DIR"
TS="$(date +%Y%m%d_%H%M%S)"
LOG="$LOG_DIR/sift1m_${PROFILE}_${CACHE_MODE}_${TS}.log"

echo "Running: profile=$PROFILE cache_mode=$CACHE_MODE"
"$BIN" "$CFG" > "$LOG" 2>&1

echo "Log: $LOG"
rg -n "actuallQPS|Recall@10|Loaded [0-9]+ learned budget models" "$LOG" | tail -n 20 || true
