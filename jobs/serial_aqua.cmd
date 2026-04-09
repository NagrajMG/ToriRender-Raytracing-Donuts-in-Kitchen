#!/bin/bash

#PBS -N torirender_serial
#PBS -l select=1:ncpus=1
#PBS -l walltime=02:00:00
#PBS -j oe
#PBS -o torirender_serial.$PBS_JOBID.log

set -euo pipefail

# PBS provides submit directory via PBS_O_WORKDIR
if [[ -z "${PBS_O_WORKDIR:-}" ]]; then
  echo "PBS_O_WORKDIR is not set. Submit this script with qsub."
  exit 1
fi

# Cluster paths and job metadata
WORKDIR="${PBS_O_WORKDIR}"
JOB_ID="${PBS_JOBID:-manual_$$}"
REPO_NAME="$(basename "${WORKDIR}")"
SCRATCH_ROOT="${HOME}/scratch"
SCRATCH_PROJECT_DIR="${SCRATCH_ROOT}/${REPO_NAME}"
SCRATCH_JOB_DIR="${SCRATCH_PROJECT_DIR}/job${JOB_ID}"
SCRATCH_REPO="${SCRATCH_JOB_DIR}"


mkdir -p "${SCRATCH_JOB_DIR}"

# Copy project into scratch to avoid heavy I/O in shared workdir.
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

# Timestamped logs and run-summary path
CONFIG_PATH="config/scene.json"
OUTPUT_DIR="output"
RUN_DATE="$(date +%Y-%m-%d)"
RUN_TIME="$(date +%Hh%Mm%Ss)"
RUN_TS="${RUN_DATE}_time_${RUN_TIME}"
RUN_ID="aqua_serial_${RUN_TS}"
STDOUT_LOG="logs/${RUN_ID}.stdout.log"
STDERR_LOG="logs/${RUN_ID}.stderr.log"
RUN_SUMMARY_LOG="results/aqua_serial_runs.log"

# Track CSV row count to verify append behavior.
METRICS_CSV="${OUTPUT_DIR}/render_metrics.csv"
before_lines=0
if [[ -f "${METRICS_CSV}" ]]; then
  before_lines="$(wc -l < "${METRICS_CSV}")"
fi

# Configure and build in serial mode
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DTORIRENDER_ENABLE_MPI=OFF \
  -DTORIRENDER_ENABLE_OPENMP=OFF

cmake --build build --target render_scene -j1

# Execute renderer and capture stdout/stderr.
start_epoch="$(date +%s)"
set +e
./build/render_scene "${CONFIG_PATH}" "${OUTPUT_DIR}" >"${STDOUT_LOG}" 2>"${STDERR_LOG}"
status=$?
set -e
end_epoch="$(date +%s)"
elapsed_seconds="$((end_epoch - start_epoch))"

# Track CSV row count after run
after_lines=0
if [[ -f "${METRICS_CSV}" ]]; then
  after_lines="$(wc -l < "${METRICS_CSV}")"
fi

# Check if a new CSV row was appended
csv_appended="no"
if [[ ${after_lines} -gt ${before_lines} ]]; then
  csv_appended="yes"
fi

# Append run metadata
{
  echo "timestamp=${RUN_TS}"
  echo "date=${RUN_DATE}"
  echo "time=${RUN_TIME}"
  echo "mode=aqua_serial"
  echo "job_id=${JOB_ID}"
  echo "status=${status}"
  echo "elapsed_seconds=${elapsed_seconds}"
  echo "workdir=${WORKDIR}"
  echo "scratch_root=${SCRATCH_ROOT}"
  echo "scratch_project_dir=${SCRATCH_PROJECT_DIR}"
  echo "scratch_job_dir=${SCRATCH_JOB_DIR}"
  echo "scratch_repo=${SCRATCH_REPO}"
  echo "config_path=${CONFIG_PATH}"
  echo "output_dir=${OUTPUT_DIR}"
  echo "metrics_csv=${METRICS_CSV}"
  echo "csv_appended=${csv_appended}"
  echo "stdout_log=${STDOUT_LOG}"
  echo "stderr_log=${STDERR_LOG}"
  echo "---"
} >>"${RUN_SUMMARY_LOG}"

# Ensure destination folders 
mkdir -p "${WORKDIR}/logs" "${WORKDIR}/results" "${WORKDIR}/output"

# Copy back only required artifacts from scratch.
if command -v rsync >/dev/null 2>&1; then
  rsync -a "${SCRATCH_REPO}/logs/" "${WORKDIR}/logs/"
  rsync -a "${SCRATCH_REPO}/results/" "${WORKDIR}/results/"
  rsync -a "${SCRATCH_REPO}/output/" "${WORKDIR}/output/"
else
  cp -a "${SCRATCH_REPO}/logs/." "${WORKDIR}/logs/"
  cp -a "${SCRATCH_REPO}/results/." "${WORKDIR}/results/"
  cp -a "${SCRATCH_REPO}/output/." "${WORKDIR}/output/"
fi

# Clean scratch on success; keeping it on failure
if [[ ${status} -eq 0 ]]; then
  rm -rf "${SCRATCH_JOB_DIR}"
else
  echo "Render failed. Scratch retained at: ${SCRATCH_JOB_DIR}"
fi

# Propagate render failure to PBS job status
if [[ ${status} -ne 0 ]]; then
  exit "${status}"
fi

# Treating missing CSV append as workflow error
if [[ "${csv_appended}" != "yes" ]]; then
  echo "Render completed but metrics CSV was not appended."
  exit 2
fi

echo "AQuA serial run completed successfully."
