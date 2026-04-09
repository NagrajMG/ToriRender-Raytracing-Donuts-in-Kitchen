#!/bin/bash

#PBS -N torirender_serial
#PBS -l select=1:ncpus=1
#PBS -l walltime=02:00:00
#PBS -j oe
#PBS -o /dev/null
#PBS -V

set -euo pipefail

# Some site profile scripts reference this without guarding under nounset.
export COLORTERM="${COLORTERM-}"

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
RENDER_STDOUT_LOG_REL="logs/${RUN_ID}.stdout.log"
RENDER_STDERR_LOG_REL="logs/${RUN_ID}.stderr.log"
RUN_SUMMARY_LOG_REL="results/aqua_serial_runs.log"

CONFIG_PATH="config/scene.json"
OUTPUT_DIR_NAME="output"
METRICS_CSV_REL="${OUTPUT_DIR_NAME}/render_metrics.csv"

cleanup_scheduler_wrappers() {
  rm -f \
    "${WORKDIR}/torirender_serial.o${JOB_ID_SHORT}" \
    "${WORKDIR}/torirender_serial.o${JOB_ID_FULL}" \
    "${WORKDIR}/torirender_serial.e${JOB_ID_SHORT}" \
    "${WORKDIR}/torirender_serial.e${JOB_ID_FULL}" \
    "${WORKDIR}/torirender_serial.\$PBS_JOBID.log"
}
trap cleanup_scheduler_wrappers EXIT

# Route full job-script output to deterministic per-run PBS log.
exec >"${PBS_LOG}" 2>&1

echo "AQuA serial job started"
echo "job_id=${JOB_ID_FULL}"
echo "workdir=${WORKDIR}"
echo "scratch_job_dir=${SCRATCH_JOB_DIR}"
echo "pbs_log=${PBS_LOG}"

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
mkdir -p logs results output

safe_source() {
  local file="$1"
  if [[ -f "${file}" ]]; then
    set +u
    # shellcheck disable=SC1090
    source "${file}" || true
    set -u
  fi
}

# Try to initialize modules in non-interactive PBS shell.
safe_source /etc/profile.d/modules.sh
safe_source /usr/share/Modules/init/bash

# If tools are missing, attempt to load common module names.
if ! command -v cmake >/dev/null 2>&1 || ! command -v c++ >/dev/null 2>&1; then
  if command -v module >/dev/null 2>&1; then
    for cmake_mod in cmake cmake3.30 cmake3.26 cmake3.20 cmake3.18 cmake3.14; do
      module load "${cmake_mod}" >/dev/null 2>&1 && break
    done
    for gcc_mod in gcc13.3.0 gcc13.1.0 gcc12.3.0 gcc10.3.0 gcc10.1.0 gcc920 gcc640 gcc; do
      module load "${gcc_mod}" >/dev/null 2>&1 && break
    done
  fi
fi

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

echo "cmake_bin=${CMAKE_BIN:-missing}"
echo "cxx_bin=${CXX_BIN:-missing}"

before_lines=0
if [[ -f "${METRICS_CSV_REL}" ]]; then
  before_lines="$(wc -l < "${METRICS_CSV_REL}")"
fi

status=0
elapsed_seconds=0

if [[ -z "${CMAKE_BIN}" ]]; then
  status=127
  : >"${RENDER_STDOUT_LOG_REL}"
  {
    echo "cmake not found in PATH."
    echo "On AQuA, load one of:"
    echo "  module load cmake3.30"
    echo "  module load cmake3.26"
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
  start_epoch="$(date +%s)"
  set +e
  ./build/render_scene "${CONFIG_PATH}" "${OUTPUT_DIR_NAME}" >"${RENDER_STDOUT_LOG_REL}" 2>"${RENDER_STDERR_LOG_REL}"
  status=$?
  set -e
  end_epoch="$(date +%s)"
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

{
  echo "timestamp=${RUN_TS}"
  echo "date=${RUN_DATE}"
  echo "time=${RUN_TIME}"
  echo "mode=aqua_serial"
  echo "job_id=${JOB_ID_FULL}"
  echo "status=${status}"
  echo "elapsed_seconds=${elapsed_seconds}"
  echo "pbs_log=${PBS_LOG}"
  echo "workdir=${WORKDIR}"
  echo "scratch_root=${SCRATCH_ROOT}"
  echo "scratch_job_dir=${SCRATCH_JOB_DIR}"
  echo "scratch_repo=${SCRATCH_REPO}"
  echo "config_path=${CONFIG_PATH}"
  echo "output_dir=${OUTPUT_DIR_NAME}"
  echo "metrics_csv=${METRICS_CSV_REL}"
  echo "csv_appended=${csv_appended}"
  echo "stdout_log=${RENDER_STDOUT_LOG_REL}"
  echo "stderr_log=${RENDER_STDERR_LOG_REL}"
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
  echo "Scratch retained at: ${SCRATCH_JOB_DIR}"
  exit "${status}"
fi

if [[ "${csv_appended}" != "yes" ]]; then
  echo "Render completed but metrics CSV was not appended."
  echo "Scratch retained at: ${SCRATCH_JOB_DIR}"
  exit 2
fi

rm -rf "${SCRATCH_JOB_DIR}"
echo "AQuA serial run completed successfully."
echo "Outputs copied back to: ${WORKDIR}"
