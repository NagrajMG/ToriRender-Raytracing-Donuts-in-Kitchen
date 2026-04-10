#!/usr/bin/env bash

# Fail on command errors, undefined vars, or pipeline failures.
set -euo pipefail

# Resolve repository root from script location.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SCRIPT_START_EPOCH="$(date +%s)"
SCRIPT_START_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"

# Positional args:
#   $1 : config path
#   $2 : output directory
CONFIG_PATH="config/scene.json"
OUTPUT_DIR="output"
if (($# > 2)); then
  echo "Usage: bash scripts/local_run.sh [config_path] [output_dir]"
  exit 1
fi
if (($# >= 1)); then
  CONFIG_PATH="$1"
fi
if (($# >= 2)); then
  OUTPUT_DIR="$2"
fi

build_divider() {
  local width="$1"
  printf '%*s' "${width}" '' | tr ' ' '='
}

center_line() {
  local width="$1"
  local text="$2"
  local len="${#text}"
  if ((len >= width)); then
    printf '%s\n' "${text}"
    return
  fi
  local left_padding=$(((width - len) / 2))
  local right_padding=$((width - len - left_padding))
  printf '%*s%s%*s\n' "${left_padding}" '' "${text}" "${right_padding}" ''
}

display_path() {
  local path="$1"
  if [[ "${path}" == "${REPO_ROOT}/"* ]]; then
    printf '%s' "${path#${REPO_ROOT}/}"
  elif [[ "${path}" == /* ]]; then
    printf '[private]'
  else
    printf '%s' "${path}"
  fi
}

BUILD_DIR="${REPO_ROOT}/build"
LOG_DIR="${REPO_ROOT}/logs"
RESULTS_DIR="${REPO_ROOT}/results"

mkdir -p "${LOG_DIR}" "${RESULTS_DIR}"

# Timestamped logs for this run.
RUN_DATE="$(date +%Y-%m-%d)"
RUN_TIME="$(date +%Hh%Mm%Ss)"
RUN_TS="${RUN_DATE}_time_${RUN_TIME}"
RUN_ID="local_serial_${RUN_TS}"
STDOUT_LOG="${LOG_DIR}/${RUN_ID}.stdout.log"
STDERR_LOG="${LOG_DIR}/${RUN_ID}.stderr.log"
RUN_SUMMARY_LOG="${RESULTS_DIR}/local_serial_runs.log"

cd "${REPO_ROOT}"

# Resolve metrics CSV path
OUTPUT_PATH="${OUTPUT_DIR}"
if [[ "${OUTPUT_PATH}" != /* ]]; then
  OUTPUT_PATH="${REPO_ROOT}/${OUTPUT_DIR}"
fi
METRICS_CSV="${OUTPUT_PATH}/render_metrics.csv"
RUN_REPORTS_DIR="${OUTPUT_PATH}/run_reports"
RUN_REPORT="${RUN_REPORTS_DIR}/${RUN_ID}.txt"
mkdir -p "${RUN_REPORTS_DIR}"

METRICS_CSV_DISPLAY="$(display_path "${METRICS_CSV}")"
STDOUT_LOG_DISPLAY="$(display_path "${STDOUT_LOG}")"
STDERR_LOG_DISPLAY="$(display_path "${STDERR_LOG}")"
RUN_REPORT_DISPLAY="$(display_path "${RUN_REPORT}")"

# Snapshot CSV line count before running, to verify append behavior.
before_lines=0
if [[ -f "${METRICS_CSV}" ]]; then
  before_lines="$(wc -l < "${METRICS_CSV}")"
fi

# Configure and build in serial-only mode
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DTORIRENDER_ENABLE_MPI=OFF \
  -DTORIRENDER_ENABLE_OPENMP=OFF

cmake --build "${BUILD_DIR}" --target render_scene -j1

# Run renderer and capture stdout/stderr in separate files.
RENDER_START_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"
start_epoch="$(date +%s)"
set +e
"${BUILD_DIR}/render_scene" "${CONFIG_PATH}" "${OUTPUT_DIR}" >"${STDOUT_LOG}" 2>"${STDERR_LOG}"
status=$?
set -e
end_epoch="$(date +%s)"
RENDER_END_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"
elapsed_seconds="$((end_epoch - start_epoch))"

# Snapshot CSV line count after running.
after_lines=0
if [[ -f "${METRICS_CSV}" ]]; then
  after_lines="$(wc -l < "${METRICS_CSV}")"
fi

# Check whether run appended a new metrics row.
csv_appended="no"
if [[ ${after_lines} -gt ${before_lines} ]]; then
  csv_appended="yes"
fi

SCRIPT_END_EPOCH="$(date +%s)"
SCRIPT_END_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"
SCRIPT_ELAPSED_SECONDS="$((SCRIPT_END_EPOCH - SCRIPT_START_EPOCH))"

latest_metrics_row="N/A"
if [[ -f "${METRICS_CSV}" ]]; then
  latest_metrics_row="$(tail -n 1 "${METRICS_CSV}" 2>/dev/null || echo "N/A")"
fi

run_status_text="SUCCESS"
if [[ ${status} -ne 0 ]]; then
  run_status_text="FAILED (exit ${status})"
fi

csv_status_text="NO"
if [[ "${csv_appended}" == "yes" ]]; then
  csv_status_text="YES"
fi

REPORT_WIDTH=160
REPORT_DIVIDER="$(build_divider "${REPORT_WIDTH}")"

{
  echo "${REPORT_DIVIDER}"
  center_line "${REPORT_WIDTH}" "ToriRender Run Report"
  echo "${REPORT_DIVIDER}"
  echo
  echo "[Run Information]"
  printf "  %-26s %s\n" "Run ID:" "${RUN_ID}"
  printf "  %-26s %s\n" "Type:" "local"
  printf "  %-26s %s\n" "Status:" "${run_status_text}"
  echo
  echo "[Timing]"
  printf "  %-26s %s\n" "Script Start (Machine):" "${SCRIPT_START_TIME}"
  printf "  %-26s %s\n" "Script End (Machine):" "${SCRIPT_END_TIME}"
  printf "  %-26s %ss\n" "Script Duration:" "${SCRIPT_ELAPSED_SECONDS}"
  printf "  %-26s %s\n" "Render Start (Machine):" "${RENDER_START_TIME}"
  printf "  %-26s %s\n" "Render End (Machine):" "${RENDER_END_TIME}"
  printf "  %-26s %ss\n" "Render Duration:" "${elapsed_seconds}"
  echo
  echo "[Inputs and Outputs]"
  printf "  %-26s %s\n" "Config Path:" "${CONFIG_PATH}"
  printf "  %-26s %s\n" "Output Directory:" "${OUTPUT_DIR}"
  printf "  %-26s %s\n" "Metrics CSV:" "${METRICS_CSV_DISPLAY}"
  printf "  %-26s %s\n" "CSV Row Appended:" "${csv_status_text}"
  printf "  %-26s %s\n" "Latest Metrics Row:" "${latest_metrics_row}"
  echo
  echo "[Logs]"
  printf "  %-26s %s\n" "Render STDOUT:" "${STDOUT_LOG_DISPLAY}"
  printf "  %-26s %s\n" "Render STDERR:" "${STDERR_LOG_DISPLAY}"
  echo
  echo "${REPORT_DIVIDER}"
} >"${RUN_REPORT}"

# Append run summary
{
  echo "timestamp=${RUN_TS}"
  echo "mode=local_serial"
  echo "status=${status}"
  echo "elapsed_seconds=${elapsed_seconds}"
  echo "config_path=${CONFIG_PATH}"
  echo "output_dir=${OUTPUT_DIR}"
  echo "metrics_csv=${METRICS_CSV_DISPLAY}"
  echo "csv_appended=${csv_appended}"
  echo "stdout_log=${STDOUT_LOG_DISPLAY}"
  echo "stderr_log=${STDERR_LOG_DISPLAY}"
  echo "run_report=${RUN_REPORT_DISPLAY}"
  echo "---"
} >>"${RUN_SUMMARY_LOG}"

# Propagate render failure with log pointers.
if [[ ${status} -ne 0 ]]; then
  echo "Render failed. Check:"
  echo "  ${STDOUT_LOG_DISPLAY}"
  echo "  ${STDERR_LOG_DISPLAY}"
  echo "  ${RUN_REPORT_DISPLAY}"
  exit "${status}"
fi

# Treat missing CSV append as workflow error.
if [[ "${csv_appended}" != "yes" ]]; then
  echo "Render completed but metrics CSV was not appended: ${METRICS_CSV_DISPLAY}"
  echo "Run report: ${RUN_REPORT_DISPLAY}"
  exit 2
fi

# Success summary
echo "Render completed."
echo "  stdout: ${STDOUT_LOG_DISPLAY}"
echo "  stderr: ${STDERR_LOG_DISPLAY}"
echo "  metrics: ${METRICS_CSV_DISPLAY}"
echo "  report: ${RUN_REPORT_DISPLAY}"
