# ToriRender

ToriRender is a torus-focused ray tracer with a unified runtime engine for:
- local serial execution
- CPU parallel execution (MPI + OpenMP)
- GPU execution (MPI + OpenACC)

The rendering physics stays common across modes; only runtime backend/orchestration changes.

## 1. Build Targets

Primary executables:
- `torirender_cpu`: serial or parallel CPU runner
- `torirender_gpu`: GPU runner (parallel)
- `render_scene`: legacy render utility
- `point_classifier`: torus implicit classifier utility

## 2. Scene Configuration (`config/scene.json`)

All fields are optional. Missing values fall back to built-in defaults.

### 2.1 Camera

| Key | Purpose | Typical values |
|---|---|---|
| `camera.look_from` | Camera position | `[0.0, 1.2, 2.5]` |
| `camera.look_at` | Target point | `[0.0, 0.0, -3.0]` |
| `camera.view_up` | Up vector | `[0.0, 1.0, 0.0]` |
| `camera.vfov` | Vertical FOV in degrees | `35` to `60` |
| `camera.image_width` | Output width | `640`, `1280`, `2560` |
| `camera.image_height` | Output height | `360`, `720`, `1440` |
| `camera.samples_per_pixel` | Anti-aliasing samples | `1` to `8+` |
| `camera.max_depth` | Reflection depth | `2` to `12` |
| `camera.rng_seed` | Deterministic seed | any integer |

### 2.2 Runtime

| Key | Purpose |
|---|---|
| `runtime.mode` | `serial` or `parallel` |
| `runtime.mpi_ranks` | Requested rank count |
| `runtime.omp_threads` | Requested threads per rank |
| `runtime.heartbeat_seconds` | Status write interval |

### 2.3 Torus Blocks

These keys exist under both `torus_primary` and `torus_secondary`.

| Key | Purpose |
|---|---|
| `major_radius` | Major radius `R` |
| `minor_radius` | Minor radius `r` |
| `center` | Torus center |
| `axis` | `"x"`, `"y"`, `"z"`, or `"custom"` |
| `axis_direction` | Required for `axis = "custom"` |
| `material` | Material block for this torus |

### 2.4 Material Block

Used by torus and floor materials.

| Key | Purpose |
|---|---|
| `type` | `"metal"` or `"diffuse"` |
| `base_color` | Base color |
| `specular_color` | Specular tint |
| `ambient`, `diffuse`, `specular` | Lighting weights |
| `shininess` | Highlight sharpness |
| `fuzz` | Metal reflection blur |
| `reflection` | Extra mirror blend |

### 2.5 Scene/Floor

| Key | Purpose |
|---|---|
| `scene.light_direction` | Main directional light |
| `scene.background_low`, `scene.background_high` | Sky gradient colors |
| `scene.floor_y` | Floor height |
| `scene.floor_checker_scale` | Checker frequency |
| `scene.floor_light_material` | Light tile material |
| `scene.floor_dark_material` | Dark tile material |

## 3. Local Run

### 3.1 Scripted (recommended)

```bash
bash scripts/local_run.sh [config_path] [output_dir]
```

Example:

```bash
bash scripts/local_run.sh config/scene.json output
```

### 3.2 Manual

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target torirender_cpu -j
./build/torirender_cpu config/scene.json output --mode serial --mpi-ranks 1 --omp-threads 1
```

Profiling-enabled manual example:

```bash
./build/torirender_cpu config/scene.json final \
  --mode parallel --mpi-ranks 8 --omp-threads 5 \
  --profile --perf-csv results/perf/metrics.csv --run-label cpu_8x5
```

## 4. AQuA Job Runs

### 4.1 CPU Job

```bash
qsub jobs/cpu.cmd
```

Parallel override example:

```bash
qsub -v MODE=parallel,MPI_RANKS=8,OMP_THREADS=1,CONFIG_PATH=config/scene.json,OUTPUT_DIR_NAME=output jobs/cpu.cmd
```

Profiling override example:

```bash
qsub -v MODE=parallel,MPI_RANKS=8,OMP_THREADS=5,PROFILE=1,PERF_CSV_PATH=results/perf/metrics.csv,RUN_LABEL=cpu_8x5 jobs/cpu.cmd
```

### 4.2 GPU Job

```bash
qsub jobs/gpu.cmd
```

Override example:

```bash
qsub -v MPI_RANKS=4,OPENACC_GPU_ARCH=ccall,OPENACC_REPORT=0,CONFIG_PATH=config/scene.json,OUTPUT_DIR_NAME=final jobs/gpu.cmd
```

Profiling override example:

```bash
qsub -v MPI_RANKS=4,PROFILE=1,PERF_CSV_PATH=results/perf/metrics.csv,RUN_LABEL=gpu_4x1 jobs/gpu.cmd
```

Rule: set `MPI_RANKS == ngpus` (1 rank per GPU).

## 5. Output Structure

Generated under your selected output directory (default `final/`):

- `images/`
  - `<backend>_<WxH>_ssp<S>_depth<D>_<YYYY-MM-DD>_time_<HHhMMmSSs>.png`
- `status/`
  - `<run_id>.status`
- `serial_metrics.csv`
  - run-level render summary (serial mode)
- `parallel_metrics.csv`
  - run-level render summary (parallel mode)
- `resource_metrics.csv`
  - per-rank resource rows (MPI/GPU/CPU/timing/workload)
- `results/perf/metrics.csv` (when `--profile` is used)
  - run-level lecture-model/performance decomposition row
- `run_reports/`
  - `<run_id>.txt` (job/run report style)
  - `resource_report_<run_id>.txt` (resource analysis report)

## 6. Metrics Reference

### 6.1 `serial_metrics.csv` and `parallel_metrics.csv`

Columns:
- `run_id,timestamp,backend,mode,image_file,resolution,ssp,depth,mpi_ranks,omp_threads,gpus,time_seconds`

### 6.2 `resource_metrics.csv`

Columns:
- `run_id,rank,hostname,gpu_id,total_ranks,ncpus,ngpus,omp_threads,scene_time,kernel_time,transfer_time,mpi_time,output_time,total_time,rays_processed`

### 6.3 `results/perf/metrics.csv`

Generated only when `--profile` is enabled and build option `TORIRENDER_ENABLE_PROFILING=ON`.

Key fields include:
- run identity/config: `timestamp,mode,image_width,image_height,samples_per_pixel,max_depth,mpi_ranks,omp_threads,p_effective,scene_file,output_file,git_commit_if_available`
- measured sections: `total_wall_seconds,sigma_setup_seconds,scene_parse_seconds,bvh_build_seconds,camera_setup_seconds,render_region_wall_seconds,mpi_* ,output_write_seconds,synchronization_seconds,finalization_seconds`
- model terms: `Ts_serial_baseline_seconds,sigma_seconds,phi_serial_seconds,ideal_phi_over_p_seconds,kappa_estimated_seconds`
- scaling: `speedup,efficiency,amdahl_ideal_speedup_from_measured_sigma,karp_flatt_e`

Analysis command:

```bash
python scripts/analyze_perf.py --input results/perf/metrics.csv --outdir results/perf
```

Outputs:
- `results/perf/performance_analysis.csv`
- `results/perf/performance_summary.md`
- `results/perf/plots/time_decomposition_stacked_bar.png`
- `results/perf/plots/overhead_breakdown_stacked_bar.png`
- `results/perf/plots/speedup_vs_processors.png`
- `results/perf/plots/efficiency_vs_processors.png`
- `results/perf/plots/karp_flatt_vs_processors.png`
- `results/perf/plots/total_time_vs_processors.png`

## 7. Quick Quality Presets

| Goal | Suggested camera settings |
|---|---|
| Fast preview | `640x360`, `samples_per_pixel=1`, `max_depth=2` |
| Better edges | `960x540`, `samples_per_pixel=4`, `max_depth=2` |
| Cleaner highlights | `1280x720`, `samples_per_pixel=8`, `max_depth=3` |

## 8. Notes

- `SceneConfig` is not a separate executable; it is loaded internally by runner binaries.
- You only pass JSON path to `torirender_cpu` / `torirender_gpu` or PBS jobs.
- Build option: `-DTORIRENDER_ENABLE_PROFILING=ON|OFF` controls profiling instrumentation availability.
- Runtime profile flags: `--profile --perf-csv <path> --run-label <text> [--profile-per-rank]`.
- Keep `logs/`, `results/`, and generated `final/` artifacts out of commits unless needed.
