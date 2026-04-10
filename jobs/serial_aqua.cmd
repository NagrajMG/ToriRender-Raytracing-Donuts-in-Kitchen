#!/bin/bash

# This script runs one simple serial render job on AQuA cluster.
# Only one CPU is used, no parallel run here.
# Flow in short:
# 1) code is copied from repo folder to scratch folder
# 2) build and render run happens in scratch
# 3) stdout/stderr/pbs logs are saved
# 4) image, csv, and run report are copied back to repo folder
# 5) if run is success scratch is cleaned, if fail scratch is kept for debugging

# Functionality:
# - for submission: qsub jobs/serial_aqua.cmd
# - Get final image in: output/images/
# - Get timing row in: output/render_metrics.csv
# - Get full run report in: output/run_reports/
# - Get logs in: logs/

#PBS -N torirender_serial
#PBS -l select=1:ncpus=1
#PBS -l walltime=24:00:00
#PBS -j oe
#PBS -o /dev/null

set -euo pipefail

export COLORTERM="${COLORTERM-}"
SCRIPT_START_EPOCH="$(date +%s)"
SCRIPT_START_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"

if [[ -z "${PBS_O_WORKDIR:-}" ]]; then
  echo "PBS_O_WORKDIR is not set. Submit this script with qsub."
  exit 1
fi

WORKDIR="${PBS_O_WORKDIR}"
JOB_ID_FULL="${PBS_JOBID:-manual_$$}"
JOB_ID_SHORT="${JOB_ID_FULL%%.*}"
REPO_NAME="$(basename "${WORKDIR}")"

SCRATCH_ROOT="${HOME}/scratch"
SCRATCH_JOB_DIR="${SCRATCH_ROOT}/${REPO_NAME}/job${JOB_ID_SHORT}"
SCRATCH_REPO="${SCRATCH_JOB_DIR}/repo"

LOG_DIR_WORK="${WORKDIR}/logs"
RESULTS_DIR_WORK="${WORKDIR}/results"
OUTPUT_DIR_WORK="${WORKDIR}/output"
mkdir -p "${LOG_DIR_WORK}" "${RESULTS_DIR_WORK}" "${OUTPUT_DIR_WORK}"

RUN_DATE="$(date +%Y-%m-%d)"
RUN_TIME="$(date +%Hh%Mm%Ss)"
RUN_TS="${RUN_DATE}_time_${RUN_TIME}"
RUN_ID="aqua_serial_${RUN_TS}"

PBS_LOG="${LOG_DIR_WORK}/${RUN_ID}.pbs.log"
PBS_LOG_REL="logs/${RUN_ID}.pbs.log"
RENDER_STDOUT_LOG_REL="logs/${RUN_ID}.stdout.log"
RENDER_STDERR_LOG_REL="logs/${RUN_ID}.stderr.log"
RUN_SUMMARY_LOG_REL="results/aqua_serial_runs.log"
RUN_REPORT_DIR_REL="output/run_reports"
RUN_REPORT_FILE_REL="${RUN_REPORT_DIR_REL}/${RUN_ID}.txt"

CONFIG_PATH="config/scene.json"
OUTPUT_DIR_NAME="output"
METRICS_CSV_REL="${OUTPUT_DIR_NAME}/render_metrics.csv"

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

cleanup_scheduler_wrappers() {
  rm -f \
    "${WORKDIR}/torirender_serial.o${JOB_ID_SHORT}" \
    "${WORKDIR}/torirender_serial.o${JOB_ID_FULL}" \
    "${WORKDIR}/torirender_serial.e${JOB_ID_SHORT}" \
    "${WORKDIR}/torirender_serial.e${JOB_ID_FULL}" \
    "${WORKDIR}/torirender_serial.\$PBS_JOBID.log"
}
trap cleanup_scheduler_wrappers EXIT

# Route job-script output to PBS log.
exec >"${PBS_LOG}" 2>&1

echo "AQuA serial job started"
echo "job_id=${JOB_ID_FULL}"
echo "workdir=[private]"
echo "scratch_job_dir=[private]"
echo "pbs_log=${PBS_LOG_REL}"

mkdir -p "${SCRATCH_JOB_DIR}"

if command -v rsync >/dev/null 2>&1; then
  rsync -a --delete \
    --exclude ".git" \
    --exclude "build" \
    --exclude "logs" \
    --exclude "results" \
    "${WORKDIR}/" "${SCRATCH_REPO}/"
else
  rm -rf "${SCRATCH_REPO}"
  mkdir -p "${SCRATCH_REPO}"
  cp -a "${WORKDIR}/." "${SCRATCH_REPO}/"
fi

cd "${SCRATCH_REPO}"
mkdir -p logs results output "${RUN_REPORT_DIR_REL}"

safe_source() {
  local file="$1"
  if [[ -f "${file}" ]]; then
    set +u
    # shellcheck disable=SC1090
    source "${file}" || true
    set -u
  fi
}

safe_source /etc/profile.d/modules.sh
safe_source /usr/share/Modules/init/bash

# Load toolchain in a clean module environment.
if command -v module >/dev/null 2>&1; then
  module purge >/dev/null 2>&1 || true
  for cmake_mod in cmake3.26 cmake3.20 cmake3.30 cmake; do
    module load "${cmake_mod}" >/dev/null 2>&1 && break
  done
  for gcc_mod in gcc13.3.0 gcc13.1.0 gcc12.3.0 gcc10.3.0 gcc10.1.0 gcc920 gcc640 gcc; do
    module load "${gcc_mod}" >/dev/null 2>&1 && break
  done
fi

is_supported_cmake() {
  local cmake_bin="$1"
  if [[ -z "${cmake_bin}" ]]; then
    return 1
  fi

  local version
  set +e
  version="$("${cmake_bin}" --version 2>/dev/null | awk 'NR==1 {print $3}')"
  local status=$?
  set -e
  if [[ "${status}" -ne 0 || -z "${version}" ]]; then
    return 1
  fi

  local major="${version%%.*}"
  local rest="${version#*.}"
  local minor="${rest%%.*}"
  if [[ ! "${major}" =~ ^[0-9]+$ || ! "${minor}" =~ ^[0-9]+$ ]]; then
    return 1
  fi

  if ((major > 3)); then
    return 0
  fi
  if ((major == 3 && minor >= 20)); then
    return 0
  fi
  return 1
}

CMAKE_BIN=""
CXX_BIN=""
if command -v cmake >/dev/null 2>&1; then
  CMAKE_BIN="$(command -v cmake)"
elif command -v cmake3 >/dev/null 2>&1; then
  CMAKE_BIN="$(command -v cmake3)"
fi

if command -v c++ >/dev/null 2>&1; then
  CXX_BIN="$(command -v c++)"
elif command -v g++ >/dev/null 2>&1; then
  CXX_BIN="$(command -v g++)"
fi

# Some cmake builds on AQuA may exist in PATH but fail at runtime (missing libcrypto.so.1.1).
# Prefer a known-working cmake binary that also satisfies project minimum version.
if ! is_supported_cmake "${CMAKE_BIN}"; then
  CMAKE_BIN=""
  for candidate in \
    /lfs/sware/cmake3.26/bin/cmake \
    /lfs/sware/cmake3.20/bin/cmake \
    /lfs/sware/cmake3.30/bin/cmake \
    /usr/local/bin/cmake; do
    if [[ -x "${candidate}" ]] && is_supported_cmake "${candidate}"; then
      CMAKE_BIN="${candidate}"
      break
    fi
  done
fi

echo "cmake_bin=${CMAKE_BIN:-missing}"
echo "cxx_bin=${CXX_BIN:-missing}"

unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH || true

before_lines=0
if [[ -f "${METRICS_CSV_REL}" ]]; then
  before_lines="$(wc -l < "${METRICS_CSV_REL}")"
fi

status=0
elapsed_seconds=0
RENDER_START_TIME="N/A"
RENDER_END_TIME="N/A"

if [[ -z "${CMAKE_BIN}" ]]; then
  status=127
  : >"${RENDER_STDOUT_LOG_REL}"
  {
    echo "Supported CMake (>= 3.20) not found in PATH."
    echo "On AQuA, load one of:"
    echo "  module load cmake3.26"
    echo "  module load cmake3.20"
  } >"${RENDER_STDERR_LOG_REL}"
elif [[ -z "${CXX_BIN}" ]]; then
  status=127
  : >"${RENDER_STDOUT_LOG_REL}"
  {
    echo "C++ compiler driver (c++) not found in PATH."
    echo "Load a compiler module before qsub, for example:"
    echo "  module load gcc13.3.0"
    echo "  module load gcc12.3.0"
  } >"${RENDER_STDERR_LOG_REL}"
else
  set +e
  "${CMAKE_BIN}" -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
    -DTORIRENDER_ENABLE_MPI=OFF \
    -DTORIRENDER_ENABLE_OPENMP=OFF \
    -DTORIRENDER_BUILD_TESTS=OFF \
    -DTORIRENDER_FETCH_CATCH2=OFF \
    -DTORIRENDER_FETCH_STB=OFF
  status=$?
  set -e
fi

if [[ ${status} -eq 0 ]]; then
  set +e
  "${CMAKE_BIN}" --build build --target render_scene -j1
  status=$?
  set -e
fi

if [[ ${status} -eq 0 ]]; then
  RENDER_START_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"
  start_epoch="$(date +%s)"
  set +e
  ./build/render_scene "${CONFIG_PATH}" "${OUTPUT_DIR_NAME}" >"${RENDER_STDOUT_LOG_REL}" 2>"${RENDER_STDERR_LOG_REL}"
  status=$?
  set -e
  end_epoch="$(date +%s)"
  RENDER_END_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"
  elapsed_seconds="$((end_epoch - start_epoch))"
else
  : >"${RENDER_STDOUT_LOG_REL}"
  if [[ ! -s "${RENDER_STDERR_LOG_REL}" ]]; then
    printf 'Build/configure failed before renderer execution.\n' >"${RENDER_STDERR_LOG_REL}"
  fi
fi

after_lines=0
if [[ -f "${METRICS_CSV_REL}" ]]; then
  after_lines="$(wc -l < "${METRICS_CSV_REL}")"
fi

csv_appended="no"
if [[ ${after_lines} -gt ${before_lines} ]]; then
  csv_appended="yes"
fi

SCRIPT_END_EPOCH="$(date +%s)"
SCRIPT_END_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"
SCRIPT_ELAPSED_SECONDS="$((SCRIPT_END_EPOCH - SCRIPT_START_EPOCH))"

latest_metrics_row="N/A"
if [[ -f "${METRICS_CSV_REL}" ]]; then
  latest_metrics_row="$(tail -n 1 "${METRICS_CSV_REL}" 2>/dev/null || echo "N/A")"
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
  printf "  %-26s %s\n" "Type:" "aqua"
  printf "  %-26s %s\n" "Job ID:" "${JOB_ID_FULL}"
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
  echo "[Paths and Outputs]"
  printf "  %-26s %s\n" "Workspace:" "${REPO_NAME}"
  printf "  %-26s %s\n" "Scratch:" "[private]"
  printf "  %-26s %s\n" "Config Path:" "${CONFIG_PATH}"
  printf "  %-26s %s\n" "Output Directory:" "${OUTPUT_DIR_NAME}"
  printf "  %-26s %s\n" "Metrics CSV:" "${METRICS_CSV_REL}"
  printf "  %-26s %s\n" "CSV Row Appended:" "${csv_status_text}"
  printf "  %-26s %s\n" "Latest Metrics Row:" "${latest_metrics_row}"
  echo
  echo "[Logs]"
  printf "  %-26s %s\n" "PBS Log:" "${PBS_LOG_REL}"
  printf "  %-26s %s\n" "Render STDOUT:" "${RENDER_STDOUT_LOG_REL}"
  printf "  %-26s %s\n" "Render STDERR:" "${RENDER_STDERR_LOG_REL}"
  echo
  echo "${REPORT_DIVIDER}"
} >"${RUN_REPORT_FILE_REL}"

{
  echo "timestamp=${RUN_TS}"
  echo "date=${RUN_DATE}"
  echo "time=${RUN_TIME}"
  echo "mode=aqua_serial"
  echo "job_id=${JOB_ID_FULL}"
  echo "status=${status}"
  echo "elapsed_seconds=${elapsed_seconds}"
  echo "pbs_log=${PBS_LOG_REL}"
  echo "workdir=[private]"
  echo "scratch_root=[private]"
  echo "scratch_job_dir=[private]"
  echo "scratch_repo=[private]"
  echo "config_path=${CONFIG_PATH}"
  echo "output_dir=${OUTPUT_DIR_NAME}"
  echo "metrics_csv=${METRICS_CSV_REL}"
  echo "csv_appended=${csv_appended}"
  echo "stdout_log=${RENDER_STDOUT_LOG_REL}"
  echo "stderr_log=${RENDER_STDERR_LOG_REL}"
  echo "run_report=${RUN_REPORT_FILE_REL}"
  echo "---"
} >>"${RUN_SUMMARY_LOG_REL}"

if command -v rsync >/dev/null 2>&1; then
  rsync -a "${SCRATCH_REPO}/logs/" "${LOG_DIR_WORK}/"
  rsync -a "${SCRATCH_REPO}/results/" "${RESULTS_DIR_WORK}/"
  rsync -a "${SCRATCH_REPO}/output/" "${OUTPUT_DIR_WORK}/"
else
  cp -a "${SCRATCH_REPO}/logs/." "${LOG_DIR_WORK}/"
  cp -a "${SCRATCH_REPO}/results/." "${RESULTS_DIR_WORK}/"
  cp -a "${SCRATCH_REPO}/output/." "${OUTPUT_DIR_WORK}/"
fi

if [[ ${status} -ne 0 ]]; then
  echo "Render job failed with status ${status}."
  echo "Scratch retained for debugging."
  echo "Run report: ${RUN_REPORT_FILE_REL}"
  exit "${status}"
fi

if [[ "${csv_appended}" != "yes" ]]; then
  echo "Render completed but metrics CSV was not appended."
  echo "Scratch retained for debugging."
  echo "Run report: ${RUN_REPORT_FILE_REL}"
  exit 2
fi

rm -rf "${SCRATCH_JOB_DIR}"
echo "AQuA serial run completed successfully."
echo "Outputs copied back to repo."
echo "Run report: ${RUN_REPORT_FILE_REL}"
