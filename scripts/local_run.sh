#!/usr/bin/env bash

# Fail on command errors, undefined vars, or pipeline failures.
set -euo pipefail

# Resolve repository root from script location.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

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

# Standard directories used by local serial workflow.
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
start_epoch="$(date +%s)"
set +e
"${BUILD_DIR}/render_scene" "${CONFIG_PATH}" "${OUTPUT_DIR}" >"${STDOUT_LOG}" 2>"${STDERR_LOG}"
status=$?
set -e
end_epoch="$(date +%s)"
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

# Append run summary
{
  echo "timestamp=${RUN_TS}"
  echo "mode=local_serial"
  echo "status=${status}"
  echo "elapsed_seconds=${elapsed_seconds}"
  echo "config_path=${CONFIG_PATH}"
  echo "output_dir=${OUTPUT_DIR}"
  echo "metrics_csv=${METRICS_CSV}"
  echo "csv_appended=${csv_appended}"
  echo "stdout_log=${STDOUT_LOG}"
  echo "stderr_log=${STDERR_LOG}"
  echo "---"
} >>"${RUN_SUMMARY_LOG}"

# Propagate render failure with log pointers.
if [[ ${status} -ne 0 ]]; then
  echo "Render failed. Check:"
  echo "  ${STDOUT_LOG}"
  echo "  ${STDERR_LOG}"
  exit "${status}"
fi

# Treat missing CSV append as workflow error.
if [[ "${csv_appended}" != "yes" ]]; then
  echo "Render completed but metrics CSV was not appended: ${METRICS_CSV}"
  exit 2
fi

# Success summary.
echo "Render completed."
echo "  stdout: ${STDOUT_LOG}"
echo "  stderr: ${STDERR_LOG}"
echo "  metrics: ${METRICS_CSV}"
