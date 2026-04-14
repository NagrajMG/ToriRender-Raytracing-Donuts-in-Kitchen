#!/bin/bash

#PBS -N torirender_gpu
#PBS -j oe
#PBS -o /dev/null

set -euo pipefail

export COLORTERM="${COLORTERM-}"
SCRIPT_START_EPOCH="$(date +%s)"
SCRIPT_START_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"

if [[ -z "${PBS_O_WORKDIR:-}" ]]; then
  echo "PBS_O_WORKDIR is not set. Submit via qsub."
  exit 1
fi

WORKDIR="${PBS_O_WORKDIR}"
JOB_ID_FULL="${PBS_JOBID:-manual_$$}"
JOB_ID_SHORT="${JOB_ID_FULL%%.*}"
REPO_NAME="$(basename "${WORKDIR}")"

SCRATCH_ROOT="${SCRATCH_ROOT:-${HOME}/scratch}"
SCRATCH_JOB_DIR="${SCRATCH_ROOT}/${REPO_NAME}/job${JOB_ID_SHORT}_gpu"
SCRATCH_REPO="${SCRATCH_JOB_DIR}/repo"

LOG_DIR_WORK="${WORKDIR}/logs"
RESULTS_DIR_WORK="${WORKDIR}/results"
OUTPUT_DIR_WORK="${WORKDIR}/output"
mkdir -p "${LOG_DIR_WORK}" "${RESULTS_DIR_WORK}" "${OUTPUT_DIR_WORK}"

RUN_DATE="$(date +%Y-%m-%d)"
RUN_TIME="$(date +%Hh%Mm%Ss)"
RUN_TS="${RUN_DATE}_time_${RUN_TIME}"
RUN_ID="gpu_parallel_${RUN_TS}"

PBS_LOG="${LOG_DIR_WORK}/${RUN_ID}.pbs.log"
PBS_LOG_REL="logs/${RUN_ID}.pbs.log"
RENDER_STDOUT_LOG_REL="logs/${RUN_ID}.stdout.log"
RENDER_STDERR_LOG_REL="logs/${RUN_ID}.stderr.log"
RUN_SUMMARY_LOG_REL="results/gpu_aqua_runs.log"
RUN_REPORT_DIR_REL="output/run_reports"
RUN_REPORT_FILE_REL="${RUN_REPORT_DIR_REL}/${RUN_ID}.txt"
MPI_HOSTFILE_REL="results/${RUN_ID}.hostfile"

CONFIG_PATH="${CONFIG_PATH:-config/scene.json}"
OUTPUT_DIR_NAME="${OUTPUT_DIR_NAME:-output}"
METRICS_CSV_REL="${OUTPUT_DIR_NAME}/render_metrics_parallel.csv"
STATUS_DIR_REL="${OUTPUT_DIR_NAME}/status"
OPENACC_GPU_ARCH="${OPENACC_GPU_ARCH:-ccall}"
OPENACC_REPORT="${OPENACC_REPORT:-0}"

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

parse_hms_to_seconds() {
  local value="$1"
  if [[ ! "${value}" =~ ^([0-9]+):([0-9]{2}):([0-9]{2})$ ]]; then
    echo "-1"
    return
  fi
  local h="${BASH_REMATCH[1]}"
  local m="${BASH_REMATCH[2]}"
  local s="${BASH_REMATCH[3]}"
  echo "$((10#${h} * 3600 + 10#${m} * 60 + 10#${s}))"
}

cleanup_scheduler_wrappers() {
  rm -f \
    "${WORKDIR}/torirender_gpu.o${JOB_ID_SHORT}" \
    "${WORKDIR}/torirender_gpu.o${JOB_ID_FULL}" \
    "${WORKDIR}/torirender_gpu.e${JOB_ID_SHORT}" \
    "${WORKDIR}/torirender_gpu.e${JOB_ID_FULL}" \
    "${WORKDIR}/torirender_gpu.\$PBS_JOBID.log"
}

cleanup_on_exit() {
  cleanup_scheduler_wrappers
  rm -rf "${SCRATCH_JOB_DIR}" 2>/dev/null || true
}
trap cleanup_on_exit EXIT

exec >"${PBS_LOG}" 2>&1

QUEUE_NAME="${PBS_QUEUE:-${PBS_O_QUEUE:-gpuq}}"
if [[ "${QUEUE_NAME}" != "gpuq" ]]; then
  echo "Conservative policy check failed: GPU job must use queue gpuq."
  exit 1
fi

ALLOC_NCPUS="${PBS_NCPUS:-${NCPUS:-8}}"
if [[ ! "${ALLOC_NCPUS}" =~ ^[0-9]+$ ]] || ((ALLOC_NCPUS <= 0)); then
  echo "Invalid allocated CPU count: ${ALLOC_NCPUS}"
  exit 1
fi

ALLOC_NGPUS="${PBS_NGPUS:-}"
if [[ -z "${ALLOC_NGPUS}" ]] && command -v qstat >/dev/null 2>&1; then
  ALLOC_NGPUS="$(qstat -f "${JOB_ID_FULL}" 2>/dev/null | awk -F= '/Resource_List.ngpus/ {gsub(/ /, "", $2); print $2; exit}')"
fi
if [[ -z "${ALLOC_NGPUS}" ]]; then
  ALLOC_NGPUS="${NGPUS:-1}"
fi
if [[ ! "${ALLOC_NGPUS}" =~ ^[0-9]+$ ]] || ((ALLOC_NGPUS <= 0)); then
  echo "Invalid allocated GPU count: ${ALLOC_NGPUS}"
  exit 1
fi

REQUESTED_WALLTIME="${WALLTIME:-}"
if [[ -z "${REQUESTED_WALLTIME}" ]] && command -v qstat >/dev/null 2>&1; then
  REQUESTED_WALLTIME="$(qstat -f "${JOB_ID_FULL}" 2>/dev/null | awk -F= '/Resource_List.walltime/ {gsub(/ /, "", $2); print $2; exit}')"
fi
if [[ -z "${REQUESTED_WALLTIME}" ]]; then
  REQUESTED_WALLTIME="48:00:00"
fi

REQ_WALL_SEC="$(parse_hms_to_seconds "${REQUESTED_WALLTIME}")"
MAX_WALL_SEC="$(parse_hms_to_seconds "48:00:00")"
if ((REQ_WALL_SEC < 0 || MAX_WALL_SEC < 0 || REQ_WALL_SEC > MAX_WALL_SEC)); then
  echo "Conservative policy check failed: gpuq allows up to 48:00:00 walltime."
  exit 1
fi

if ((ALLOC_NCPUS > 40)); then
  echo "Conservative policy check failed: GPU job ncpus must be <= 40."
  exit 1
fi

MPI_RANKS="${MPI_RANKS:-${ALLOC_NGPUS}}"
if [[ ! "${MPI_RANKS}" =~ ^[0-9]+$ ]] || ((MPI_RANKS <= 0)); then
  echo "Invalid MPI_RANKS=${MPI_RANKS}"
  exit 1
fi

if ((MPI_RANKS != ALLOC_NGPUS)); then
  echo "Conservative policy check failed: torirender_gpu requires 1 MPI rank per GPU."
  exit 1
fi

OMP_THREADS="${OMP_THREADS:-1}"
if [[ ! "${OMP_THREADS}" =~ ^[0-9]+$ ]] || ((OMP_THREADS != 1)); then
  echo "torirender_gpu enforces OMP_THREADS=1."
  exit 1
fi

HEARTBEAT_SECONDS="${HEARTBEAT_SECONDS:-60}"
if [[ ! "${HEARTBEAT_SECONDS}" =~ ^[0-9]+$ ]] || ((HEARTBEAT_SECONDS <= 0)); then
  echo "Invalid HEARTBEAT_SECONDS=${HEARTBEAT_SECONDS}"
  exit 1
fi

if [[ "${OPENACC_REPORT}" != "0" && "${OPENACC_REPORT}" != "1" ]]; then
  echo "Invalid OPENACC_REPORT=${OPENACC_REPORT} (use 0 or 1)."
  exit 1
fi

OPENACC_REPORT_CMAKE="OFF"
if [[ "${OPENACC_REPORT}" == "1" ]]; then
  OPENACC_REPORT_CMAKE="ON"
fi

echo "AQuA GPU job started"
echo "job_id=${JOB_ID_FULL}"
echo "queue=${QUEUE_NAME}"
echo "mpi_ranks=${MPI_RANKS}"
echo "omp_threads=${OMP_THREADS}"
echo "ncpus=${ALLOC_NCPUS}"
echo "ngpus=${ALLOC_NGPUS}"
echo "walltime=${REQUESTED_WALLTIME}"
echo "openacc_gpu_arch=${OPENACC_GPU_ARCH}"
echo "openacc_report=${OPENACC_REPORT_CMAKE}"
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

if command -v module >/dev/null 2>&1; then
  module purge >/dev/null 2>&1 || true
  # Prefer cmake3.26 on AQuA (cmake3.30 may miss runtime libs on some nodes).
  for cmake_mod in cmake3.26 cmake3.20 cmake3.18 cmake3.14 cmake3.30; do
    module load "${cmake_mod}" >/dev/null 2>&1 && break
  done
  for gcc_mod in gcc13.3.0 gcc13.1.0 gcc12.3.0 gcc10.3.0 gcc10.1.0 gcc920 gcc640; do
    module load "${gcc_mod}" >/dev/null 2>&1 && break
  done
  for nvhpc_mod in nvhpc-25.7 nvhpc-23.5 nvhpc-21.11 nvhpc-21.7; do
    module load "${nvhpc_mod}" >/dev/null 2>&1 && break
  done
  for cuda_mod in cuda12.4 cuda12.2 cuda12.1 cuda11.7; do
    module load "${cuda_mod}" >/dev/null 2>&1 && break
  done
  if ! command -v mpirun >/dev/null 2>&1; then
    for mpi_mod in openmpi415 openmpi411 openmpi405 openmpi404 openmpi406 openmpi316 openmpi506 openmpi501; do
      module load "${mpi_mod}" >/dev/null 2>&1 && break
    done
  fi
fi

CMAKE_BIN=""
if command -v cmake >/dev/null 2>&1; then
  CMAKE_BIN="$(command -v cmake)"
elif command -v cmake3 >/dev/null 2>&1; then
  CMAKE_BIN="$(command -v cmake3)"
fi

CXX_BIN=""
if command -v nvc++ >/dev/null 2>&1; then
  CXX_BIN="$(command -v nvc++)"
fi

MPIRUN_BIN=""
if command -v mpirun >/dev/null 2>&1; then
  MPIRUN_BIN="$(command -v mpirun)"
fi

if command -v g++ >/dev/null 2>&1; then
  GXX_LIBSTDCPP="$(g++ -print-file-name=libstdc++.so.6 2>/dev/null || true)"
  if [[ -n "${GXX_LIBSTDCPP}" && "${GXX_LIBSTDCPP}" != "libstdc++.so.6" ]]; then
    GXX_LIB_DIR="$(dirname "${GXX_LIBSTDCPP}")"
    export LD_LIBRARY_PATH="${GXX_LIB_DIR}:${LD_LIBRARY_PATH:-}"
  fi
fi

echo "cmake_bin=${CMAKE_BIN:-missing}"
echo "cxx_bin=${CXX_BIN:-missing}"
echo "mpirun_bin=${MPIRUN_BIN:-missing}"

unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH || true

before_lines=0
if [[ -f "${METRICS_CSV_REL}" ]]; then
  before_lines="$(wc -l < "${METRICS_CSV_REL}")"
fi

status=0
render_elapsed=0
RENDER_START_TIME="N/A"
RENDER_END_TIME="N/A"

if [[ -z "${CMAKE_BIN}" || -z "${CXX_BIN}" ]]; then
  status=127
  : >"${RENDER_STDOUT_LOG_REL}"
  {
    if [[ -z "${CMAKE_BIN}" ]]; then
      echo "cmake not found in PATH."
    fi
    if [[ -z "${CXX_BIN}" ]]; then
      echo "nvc++ not found in PATH."
      echo "Load one NVHPC module before qsub, for example: module load nvhpc-25.7"
    fi
  } >"${RENDER_STDERR_LOG_REL}"
else
  set +e
  "${CMAKE_BIN}" -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
    -DTORIRENDER_ENABLE_MPI=ON \
    -DTORIRENDER_ENABLE_OPENMP=OFF \
    -DTORIRENDER_ENABLE_OPENACC=ON \
    -DTORIRENDER_OPENACC_GPU_ARCH="${OPENACC_GPU_ARCH}" \
    -DTORIRENDER_OPENACC_REPORT="${OPENACC_REPORT_CMAKE}" \
    -DTORIRENDER_BUILD_TESTS=OFF \
    -DTORIRENDER_FETCH_CATCH2=OFF \
    -DTORIRENDER_FETCH_STB=OFF
  status=$?
  set -e
fi

if [[ ${status} -eq 0 ]]; then
  set +e
  "${CMAKE_BIN}" --build build --target torirender_gpu -j1
  status=$?
  set -e
fi

export OMP_NUM_THREADS=1

if [[ ${status} -eq 0 ]]; then
  if [[ -z "${MPIRUN_BIN}" ]]; then
    status=127
    : >"${RENDER_STDOUT_LOG_REL}"
    echo "mpirun not found in PATH" >"${RENDER_STDERR_LOG_REL}"
  else
    RENDER_START_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"
    start_epoch="$(date +%s)"

    # Build hostfile from PBS allocation for deterministic MPI slot mapping.
    if [[ -n "${PBS_NODEFILE:-}" && -f "${PBS_NODEFILE}" ]]; then
      awk -v alloc_ngpus="${ALLOC_NGPUS}" '
        {
          host=$1;
          sub(/\/.*/, "", host);
          if (host != "") {
            count[host]++;
            first_host = (first_host == "" ? host : first_host);
          }
        }
        END {
          hosts = 0;
          total = 0;
          for (h in count) {
            hosts++;
            total += count[h];
          }

          # Some PBS setups list one line per node in PBS_NODEFILE.
          # For single-node GPU jobs, force slots to allocated ngpus.
          if (hosts == 1 && alloc_ngpus > total && first_host != "") {
            count[first_host] = alloc_ngpus;
          }

          for (h in count) {
            printf "%s slots=%d max_slots=%d\n", h, count[h], count[h];
          }
        }
      ' \
        "${PBS_NODEFILE}" > "${MPI_HOSTFILE_REL}"
    else
      printf "%s slots=%s max_slots=%s\n" "$(hostname)" "${ALLOC_NGPUS}" "${ALLOC_NGPUS}" > "${MPI_HOSTFILE_REL}"
    fi

    if [[ ! -s "${MPI_HOSTFILE_REL}" ]]; then
      printf "%s slots=%s max_slots=%s\n" "$(hostname)" "${ALLOC_NGPUS}" "${ALLOC_NGPUS}" > "${MPI_HOSTFILE_REL}"
    fi

    echo "mpi_hostfile=${MPI_HOSTFILE_REL}"
    echo "mpi_hostfile_contents:"
    cat "${MPI_HOSTFILE_REL}"

    set +e
    "${MPIRUN_BIN}" --hostfile "${MPI_HOSTFILE_REL}" \
      -np "${MPI_RANKS}" \
      --bind-to none --map-by slot \
      ./build/torirender_gpu "${CONFIG_PATH}" "${OUTPUT_DIR_NAME}" \
      --mode parallel \
      --mpi-ranks "${MPI_RANKS}" \
      --omp-threads 1 \
      --heartbeat "${HEARTBEAT_SECONDS}" \
      >"${RENDER_STDOUT_LOG_REL}" 2>"${RENDER_STDERR_LOG_REL}"
    status=$?
    set -e

    end_epoch="$(date +%s)"
    RENDER_END_TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"
    render_elapsed="$((end_epoch - start_epoch))"
  fi
else
  : >"${RENDER_STDOUT_LOG_REL}"
  if [[ ! -s "${RENDER_STDERR_LOG_REL}" ]]; then
    echo "Build/configure failed before runner execution." >"${RENDER_STDERR_LOG_REL}"
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
  printf "  %-26s %s\n" "Backend:" "gpu"
  printf "  %-26s %s\n" "Mode:" "parallel"
  printf "  %-26s %s\n" "Job ID:" "${JOB_ID_FULL}"
  printf "  %-26s %s\n" "Status:" "${run_status_text}"
  echo
  echo "[Timing]"
  printf "  %-26s %s\n" "Script Start (Machine):" "${SCRIPT_START_TIME}"
  printf "  %-26s %s\n" "Script End (Machine):" "${SCRIPT_END_TIME}"
  printf "  %-26s %ss\n" "Script Duration:" "${SCRIPT_ELAPSED_SECONDS}"
  printf "  %-26s %s\n" "Render Start (Machine):" "${RENDER_START_TIME}"
  printf "  %-26s %s\n" "Render End (Machine):" "${RENDER_END_TIME}"
  printf "  %-26s %ss\n" "Render Duration:" "${render_elapsed}"
  echo
  echo "[Runtime Configuration]"
  printf "  %-26s %s\n" "Queue:" "${QUEUE_NAME}"
  printf "  %-26s %s\n" "Allocated CPUs:" "${ALLOC_NCPUS}"
  printf "  %-26s %s\n" "Allocated GPUs:" "${ALLOC_NGPUS}"
  printf "  %-26s %s\n" "Walltime:" "${REQUESTED_WALLTIME}"
  printf "  %-26s %s\n" "MPI Ranks:" "${MPI_RANKS}"
  printf "  %-26s %s\n" "OMP Threads:" "${OMP_THREADS}"
  printf "  %-26s %s\n" "Heartbeat Seconds:" "${HEARTBEAT_SECONDS}"
  printf "  %-26s %s\n" "OpenACC GPU Arch:" "${OPENACC_GPU_ARCH}"
  printf "  %-26s %s\n" "OpenACC Report:" "${OPENACC_REPORT_CMAKE}"
  echo
  echo "[Inputs and Outputs]"
  printf "  %-26s %s\n" "Config Path:" "${CONFIG_PATH}"
  printf "  %-26s %s\n" "Output Directory:" "${OUTPUT_DIR_NAME}"
  printf "  %-26s %s\n" "Status Directory:" "${STATUS_DIR_REL}"
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
  echo "mode=aqua_gpu_parallel"
  echo "status=${status}"
  echo "elapsed_seconds=${render_elapsed}"
  echo "queue=${QUEUE_NAME}"
  echo "ncpus=${ALLOC_NCPUS}"
  echo "ngpus=${ALLOC_NGPUS}"
  echo "walltime=${REQUESTED_WALLTIME}"
  echo "mpi_ranks=${MPI_RANKS}"
  echo "omp_threads=${OMP_THREADS}"
  echo "heartbeat_seconds=${HEARTBEAT_SECONDS}"
  echo "metrics_csv=${METRICS_CSV_REL}"
  echo "csv_appended=${csv_appended}"
  echo "pbs_log=${PBS_LOG_REL}"
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
  echo "GPU job failed with status ${status}."
  echo "Scratch cleaned."
  echo "Run report: ${RUN_REPORT_FILE_REL}"
  exit "${status}"
fi

if [[ "${csv_appended}" != "yes" ]]; then
  echo "GPU job completed but metrics CSV was not appended."
  echo "Scratch cleaned."
  echo "Run report: ${RUN_REPORT_FILE_REL}"
  exit 2
fi

echo "AQuA GPU run completed successfully."
echo "Outputs copied back to repo."
echo "Run report: ${RUN_REPORT_FILE_REL}"
