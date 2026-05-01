#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_spann_concurrency_trace.sh -c <config.ini> -o <output_root> [options]

Required:
  -c, --config            Base search config
  -o, --output-root       Root output directory for all runs

Optional:
  -b, --binary            ssdserving binary path (default: ./Release/ssdserving)
  -d, --device            Disk device name
  -p, --data-path         Data/index path used for device auto-detect
  -t, --threads           SearchThreadNum list, comma-separated (default: 1,2,4,8)
  -n, --io-threads        Force NumberOfThreads for every run
  -q, --query-limit       Override QueryCountLimit
  -i, --interval-ms       Monitor sampling interval ms (default: 100)
  -m, --max-read-mbps     Device baseline read MB/s (default: 0)
  -C, --clear-cache       Drop OS page cache before each run
EOF
}

BINARY="./Release/ssdserving"
CONFIG=""
OUTPUT_ROOT=""
DEVICE=""
DATA_PATH=""
THREAD_LIST="1,2,4,8"
FORCE_NUMBER_OF_THREADS=""
QUERY_LIMIT=""
INTERVAL_MS="100"
MAX_READ_MBPS="0"
CLEAR_CACHE="false"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--binary) BINARY="$2"; shift 2 ;;
    -c|--config) CONFIG="$2"; shift 2 ;;
    -o|--output-root) OUTPUT_ROOT="$2"; shift 2 ;;
    -d|--device) DEVICE="$2"; shift 2 ;;
    -p|--data-path) DATA_PATH="$2"; shift 2 ;;
    -t|--threads) THREAD_LIST="$2"; shift 2 ;;
    -n|--io-threads) FORCE_NUMBER_OF_THREADS="$2"; shift 2 ;;
    -q|--query-limit) QUERY_LIMIT="$2"; shift 2 ;;
    -i|--interval-ms) INTERVAL_MS="$2"; shift 2 ;;
    -m|--max-read-mbps) MAX_READ_MBPS="$2"; shift 2 ;;
    -C|--clear-cache) CLEAR_CACHE="true"; shift ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$CONFIG" || -z "$OUTPUT_ROOT" ]]; then
  usage
  exit 1
fi

if [[ ! -f "$CONFIG" ]]; then
  echo "Config not found: $CONFIG" >&2
  exit 1
fi

mkdir -p "$OUTPUT_ROOT"

IFS=',' read -r -a THREADS <<< "$THREAD_LIST"
if [[ ${#THREADS[@]} -eq 0 ]]; then
  echo "Thread list is empty." >&2
  exit 1
fi

abs_output_root=$(realpath "$OUTPUT_ROOT")
baseline_query_csv=""
comparison_inputs=()

for st in "${THREADS[@]}"; do
  if [[ -z "$st" ]]; then
    continue
  fi
  if ! [[ "$st" =~ ^[0-9]+$ ]]; then
    echo "Invalid SearchThreadNum: $st" >&2
    exit 1
  fi

  run_dir="$abs_output_root/st${st}"
  config_copy="$run_dir/config.ini"
  query_csv="$run_dir/query_io_stats.csv"
  mkdir -p "$run_dir"
  cp "$CONFIG" "$config_copy"

  nt="$st"
  if [[ -n "$FORCE_NUMBER_OF_THREADS" ]]; then
    nt="$FORCE_NUMBER_OF_THREADS"
  fi
  if ! [[ "$nt" =~ ^[0-9]+$ ]]; then
    echo "Invalid NumberOfThreads override: $nt" >&2
    exit 1
  fi
  if (( nt < st )); then
    nt="$st"
  fi

  sed -i "s/^SearchThreadNum=.*/SearchThreadNum=${st}/" "$config_copy"
  sed -i "s/^NumberOfThreads=.*/NumberOfThreads=${nt}/" "$config_copy"
  sed -i "s|^DetailedIOStatsOutput=.*|DetailedIOStatsOutput=${query_csv}|" "$config_copy"
  if [[ -n "$QUERY_LIMIT" ]]; then
    if grep -q '^QueryCountLimit=' "$config_copy"; then
      sed -i "s/^QueryCountLimit=.*/QueryCountLimit=${QUERY_LIMIT}/" "$config_copy"
    else
      printf '\nQueryCountLimit=%s\n' "$QUERY_LIMIT" >> "$config_copy"
    fi
  fi

  run_args=(
    -b "$BINARY"
    -c "$config_copy"
    -o "$run_dir"
    -q "$query_csv"
    -i "$INTERVAL_MS"
    -m "$MAX_READ_MBPS"
  )
  if [[ -n "$DEVICE" ]]; then
    run_args+=(-d "$DEVICE")
  fi
  if [[ -n "$DATA_PATH" ]]; then
    run_args+=(-p "$DATA_PATH")
  fi
  if [[ "$CLEAR_CACHE" == "true" ]]; then
    run_args+=(-C)
  fi

  echo "=== Running SearchThreadNum=${st}, NumberOfThreads=${nt} ==="
  bash scripts/run_io_analysis.sh "${run_args[@]}"

  if [[ -z "$baseline_query_csv" ]]; then
    baseline_query_csv="$query_csv"
  else
    comparison_inputs+=("$query_csv")
  fi
done

if [[ -n "$baseline_query_csv" && ${#comparison_inputs[@]} -gt 0 ]]; then
  python3 scripts/compare_spann_query_hashes.py \
    --baseline "$baseline_query_csv" \
    --candidate "${comparison_inputs[@]}" | tee "$abs_output_root/hash_compare.txt"
  echo "Hash comparison saved to $abs_output_root/hash_compare.txt"
fi
