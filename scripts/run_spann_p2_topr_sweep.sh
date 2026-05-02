#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_spann_p2_topr_sweep.sh -c <base_config.ini> -o <output_root> [options]

Required:
  -c, --config              Base search config
  -o, --output-root         Root output directory for all runs

Optional:
  -b, --binary              ssdserving binary path (default: ./Release/ssdserving)
  -d, --device              Disk device name
  -p, --data-path           Data/index path used for device auto-detect
  -r, --per-posting         PostingTopRPerPosting list (default: 32,64,96,128)
  -g, --global              PostingTopRGlobal list (default: 128,256,384,512,768)
  -t, --threads             SearchThreadNum list (default: 1)
  -n, --io-threads          Force NumberOfThreads for every run
  -q, --query-limit         Override QueryCountLimit
  -i, --interval-ms         Monitor sampling interval ms (default: 100)
  -m, --max-read-mbps       Device baseline read MB/s (default: 0)
  -C, --clear-cache         Drop OS page cache before each run
  --no-monitor              Run ssdserving directly and skip /proc/disk monitor

Notes:
  This script creates a fresh output directory per combination and refuses to
  overwrite existing run outputs through scripts/run_io_analysis.sh or direct logs.
EOF
}

BINARY="./Release/ssdserving"
CONFIG=""
OUTPUT_ROOT=""
DEVICE=""
DATA_PATH=""
PER_POSTING_LIST="32,64,96,128"
GLOBAL_LIST="128,256,384,512,768"
THREAD_LIST="1"
FORCE_NUMBER_OF_THREADS=""
QUERY_LIMIT=""
INTERVAL_MS="100"
MAX_READ_MBPS="0"
CLEAR_CACHE="false"
NO_MONITOR="false"

set_ini_value() {
  local file="$1"
  local key="$2"
  local value="$3"

  if grep -q "^${key}=" "$file"; then
    sed -i "s|^${key}=.*|${key}=${value}|" "$file"
  else
    printf '%s=%s\n' "$key" "$value" >>"$file"
  fi
}

validate_int_list() {
  local label="$1"
  local list="$2"
  local item
  IFS=',' read -r -a items <<<"$list"
  for item in "${items[@]}"; do
    if [[ -z "$item" || ! "$item" =~ ^[0-9]+$ ]]; then
      echo "Invalid ${label}: ${item}" >&2
      exit 1
    fi
  done
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--binary) BINARY="$2"; shift 2 ;;
    -c|--config) CONFIG="$2"; shift 2 ;;
    -o|--output-root) OUTPUT_ROOT="$2"; shift 2 ;;
    -d|--device) DEVICE="$2"; shift 2 ;;
    -p|--data-path) DATA_PATH="$2"; shift 2 ;;
    -r|--per-posting) PER_POSTING_LIST="$2"; shift 2 ;;
    -g|--global) GLOBAL_LIST="$2"; shift 2 ;;
    -t|--threads) THREAD_LIST="$2"; shift 2 ;;
    -n|--io-threads) FORCE_NUMBER_OF_THREADS="$2"; shift 2 ;;
    -q|--query-limit) QUERY_LIMIT="$2"; shift 2 ;;
    -i|--interval-ms) INTERVAL_MS="$2"; shift 2 ;;
    -m|--max-read-mbps) MAX_READ_MBPS="$2"; shift 2 ;;
    -C|--clear-cache) CLEAR_CACHE="true"; shift ;;
    --no-monitor) NO_MONITOR="true"; shift ;;
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

if [[ ! -x "$BINARY" ]]; then
  echo "Binary not executable: $BINARY" >&2
  exit 1
fi

validate_int_list "PostingTopRPerPosting list" "$PER_POSTING_LIST"
validate_int_list "PostingTopRGlobal list" "$GLOBAL_LIST"
validate_int_list "SearchThreadNum list" "$THREAD_LIST"

mkdir -p "$OUTPUT_ROOT"
abs_output_root=$(realpath "$OUTPUT_ROOT")
matrix_file="$abs_output_root/run_matrix.tsv"

if [[ -e "$matrix_file" ]]; then
  echo "Refusing to overwrite existing matrix: $matrix_file" >&2
  exit 1
fi

printf 'run_name\tsearch_thread_num\tposting_topr_per_posting\tposting_topr_global\tconfig\tquery_csv\trun_dir\n' \
  >"$matrix_file"

IFS=',' read -r -a PER_POSTINGS <<<"$PER_POSTING_LIST"
IFS=',' read -r -a GLOBALS <<<"$GLOBAL_LIST"
IFS=',' read -r -a THREADS <<<"$THREAD_LIST"

for st in "${THREADS[@]}"; do
  for per_posting in "${PER_POSTINGS[@]}"; do
    for global_topr in "${GLOBALS[@]}"; do
      run_name="st${st}_pp${per_posting}_pg${global_topr}"
      run_dir="$abs_output_root/$run_name"
      config_copy="$run_dir/config.ini"
      query_csv="$run_dir/query_io_stats.csv"
      mkdir -p "$run_dir"
      cp "$CONFIG" "$config_copy"

      nt="$st"
      if [[ -n "$FORCE_NUMBER_OF_THREADS" ]]; then
        nt="$FORCE_NUMBER_OF_THREADS"
      fi
      if (( nt < st )); then
        nt="$st"
      fi

      set_ini_value "$config_copy" "SearchThreadNum" "$st"
      set_ini_value "$config_copy" "NumberOfThreads" "$nt"
      set_ini_value "$config_copy" "PostingTopRPerPosting" "$per_posting"
      set_ini_value "$config_copy" "PostingTopRGlobal" "$global_topr"
      set_ini_value "$config_copy" "EnableDetailedIOStats" "true"
      set_ini_value "$config_copy" "DetailedIOStatsSampleRate" "1.0"
      set_ini_value "$config_copy" "DetailedIOStatsOutput" "$query_csv"
      if [[ -n "$QUERY_LIMIT" ]]; then
        set_ini_value "$config_copy" "QueryCountLimit" "$QUERY_LIMIT"
      fi

      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$run_name" "$st" "$per_posting" "$global_topr" "$config_copy" "$query_csv" "$run_dir" >>"$matrix_file"

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

      echo "=== Running ${run_name}, NumberOfThreads=${nt} ==="
      if [[ "$NO_MONITOR" == "true" ]]; then
        log_file="$run_dir/sptag.log"
        if [[ -e "$log_file" || -e "$query_csv" ]]; then
          echo "Refusing to overwrite existing direct-run output in $run_dir" >&2
          exit 1
        fi
        "$BINARY" "$config_copy" >"$log_file" 2>&1
      else
        bash scripts/run_io_analysis.sh "${run_args[@]}"
      fi
    done
  done
done

summary_file="$abs_output_root/summary_metrics.tsv"
python3 scripts/summarize_spann_p2_sweep.py --matrix "$matrix_file" --output "$summary_file"
echo "Run matrix: $matrix_file"
echo "Summary: $summary_file"
