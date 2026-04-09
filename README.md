# ToriRender

## Configuration Guide (`config/scene.json`)
Use `config/scene.json` to control camera, geometry, materials, and render quality without touching code.  
All fields are optional; missing values fall back to built-in defaults.

### 1) Camera
| Key | Purpose | Typical values |
|---|---|---|
| `camera.look_from` | Camera position | `[0.0, 1.2, 2.5]` |
| `camera.look_at` | Target point | `[0.0, 0.0, -3.0]` |
| `camera.view_up` | Camera up vector | `[0.0, 1.0, 0.0]` |
| `camera.vfov` | Vertical field of view (degrees) | `35` to `60` |
| `camera.image_width`, `camera.image_height` | Output resolution | `640x360`, `1280x720` |
| `camera.samples_per_pixel` | Anti-aliasing samples (SSP) | `1` to `8` |
| `camera.max_depth` | Reflection bounce depth | `2` to `3` |
| `camera.rng_seed` | Deterministic random seed | any integer |

### 2) Torus Blocks
These keys exist under both `torus_primary` and `torus_secondary`.

| Key | Purpose |
|---|---|
| `major_radius` | Main ring radius (`R`) |
| `minor_radius` | Tube radius (`r`) |
| `center` | Torus center in world coordinates |
| `axis` | `"x"`, `"y"`, `"z"`, or `"custom"` |
| `axis_direction` | Required when `axis = "custom"` (slanted torus axis) |
| `material` | Material block for this torus |

### 3) Material Block
Used for each torus and floor tiles.

| Key | Purpose |
|---|---|
| `type` | `"metal"` or `"diffuse"` |
| `base_color` | Base surface color |
| `specular_color` | Highlight/reflection tint |
| `ambient`, `diffuse`, `specular` | Lighting contribution weights |
| `shininess` | Highlight sharpness |
| `fuzz` | Reflection blur (mostly for metal) |
| `reflection` | Extra mirror blend (used for diffuse floor materials) |

### 4) Scene and Floor
| Key | Purpose |
|---|---|
| `scene.light_direction` | Main directional light |
| `scene.background_low`, `scene.background_high` | Sky gradient colors |
| `scene.floor_y` | Ground plane height |
| `scene.floor_checker_scale` | Checker tile frequency |
| `scene.floor_light_material` | Material for light checker tiles |
| `scene.floor_dark_material` | Material for dark checker tiles |

### 5) Quick Quality Presets
| Goal | Suggested settings |
|---|---|
| Fast preview | `640x360`, `samples_per_pixel=1`, `max_depth=2` |
| Better edges | `960x540`, `samples_per_pixel=4`, `max_depth=2` |
| Cleaner highlights | `1280x720`, `samples_per_pixel=8`, `max_depth=3` |

### 6) Render Output Layout
Run:

```bash
./build/render_scene [config_path] [output_dir]
```

Output files:
- `<output_dir>/<image_name>.png`
- `<output_dir>/render_metrics.csv`

CSV columns:
- `resolution`
- `ssp`
- `depth`
- `time_seconds`
