# PolyMesh web application

`polymesh-webd` is the local HTTP server for the PolyMesh browser interface. It loads real CAD or STL geometry, runs the same pipeline used by the desktop and command-line applications, streams construction and solve telemetry, and serves the static web client.

## Running

From an installed release:

```sh
bin/polymesh-webd
```

The default address is `http://127.0.0.1:8770`. The server finds static files at `../share/polymesh/web` and examples at `../share/polymesh/examples`, relative to its executable. In a source checkout it falls back to `./web` and `./bench/geometries/public`.

Options:

```text
--host ADDRESS       listen address (default 127.0.0.1)
--port N             listen port (default 8770)
--web-root DIR       static frontend directory
--examples-dir DIR   installed or source example directory
--advisor DIR        exported advisor model directory
--max-mem-gb N       maximum solve footprint in decimal GB
--max-elems N        hard volume-element ceiling
--max-dof N          hard degree-of-freedom ceiling
```

A zero resource ceiling selects the pipeline's automatic/default safety limit. In particular, `--max-mem-gb 0` uses the solver's automatic memory cap (70 percent of currently available memory), while zero element and DOF ceilings select the pipeline defaults. Nonzero values are applied to every submitted job. Request bodies are limited to 256 MiB. The server retains at most 32 parts and 32 finished jobs; old records and their mesh/result payloads are evicted.

## Security posture

The server binds to loopback by default. It has no authentication, no authorization, and no tenant isolation. It is a local, single-tenant/self-hosted engineering tool, not a multi-tenant service. Binding it to a non-loopback address exposes part upload, job execution, cancellation, results, and static files to every client that can reach that address. Put an authenticating reverse proxy and network access controls in front of it before deliberately exposing it beyond a trusted host.

## Encoding convention

Large numeric arrays use base64-encoded little-endian typed arrays. Their key ends in `_b64` and the containing object includes the relevant count. Unless stated otherwise, arrays are `Float32`:

- `positions_b64`, `centroids_b64`, `color_b64`, `normals_b64`, and `disp_b64` contain three floats per vertex.
- `index_b64`, `region_b64`, `von_mises_b64`, `u_mag_b64`, and `eta_b64` contain one float per vertex.
- `edges_b64` contains three floats per edge endpoint; `n_edge_verts` counts endpoints.
- `n_verts` counts rendered vertices, not floats or bytes.

Activation vectors, network weights, scores, boxes, and other small arrays are ordinary JSON numbers.

All error responses have the shape:

```json
{"ok":false,"error":"human-readable message"}
```

## HTTP API

Schema blocks below use `N` for an observed integer, `F` for an observed finite
number, `BOOL` for a boolean, `STRING` for text, `B64` for encoded bytes, and
`ARRAY` for the stated JSON array. They are schemas, not captured example runs.

### `GET /api/health`

```text
{"ok":true,"version":STRING,"advisor":BOOL,"occ":BOOL,"threads":N}
```

The booleans report whether an advisor was loaded and whether STEP/BRep support was compiled in. `threads` is the runtime hardware-concurrency report.

### `GET /api/examples`

Lists installed STEP/BRep examples:

```json
{"ok":true,"examples":[{"id":"unit_box","name":"Unit box","path":"unit_box.step"}]}
```

### `POST /api/parts`

The raw request body is a `.step`, `.stp`, `.brep`, or `.stl` file. Its filename is supplied in `X-PolyMesh-Filename`.

### `POST /api/parts/example/{id}`

Loads one entry returned by `/api/examples`.

Both part-loading endpoints return:

```text
{"ok":true,"part":{
  "id":STRING,"name":STRING,"kind":STRING,
  "triangles":N,"regions":N,
  "bbox_min":[F,F,F],"bbox_max":[F,F,F],"suggested_h":F,
  "surface":{"n_verts":N,"positions_b64":B64,"normals_b64":B64,"region_b64":B64}
}}
```

Every reported count, bound, region, and surface sample comes from the loaded model. `suggested_h` is the model's bounding-box diagonal divided by 25.

### `POST /api/jobs`

Starts a background mesh or solve and immediately returns HTTP 202:

```text
{"part":STRING,"kind":"solve|mesh","h":F,"E":F,"nu":F,"mesher":STRING,
 "adapt_passes":N,"eta_target":F,"skin_layers":N,"feature_grading":BOOL,
 "fixtures":[[F,F,F,F,F,F],...],
 "loads":[{"box":[F,F,F,F,F,F],"force":[F,F,F]},...]}
```

```json
{"ok":true,"job":"j_..."}
```

Boxes select boundary-surface nodes, never interior nodes. Loads are energy-conjugate surface loads clipped to their boxes and conserve each requested force resultant. A solve requires at least one fixture and load. Mesher names are parsed by the pipeline's canonical `mesher_from_name` vocabulary.
`h` is metres; zero delegates automatic size selection to the pipeline.

### `GET /api/jobs/{id}/events`

Returns `text/event-stream` with `Cache-Control: no-cache` and `X-Accel-Buffering: no`. Every event is retained for the lifetime of its job, so a late or reconnected subscriber replays from `hello`. Multiple clients may observe the same job. Idle streams receive `: keepalive` every 15 seconds.

An SSE record is:

```text
event: progress
data: {"phase":"mesh",...}

```

Events occur in pipeline order:

- `hello`

  ```text
  {"job":STRING,"kind":"solve|mesh","part":STRING,"mesher":STRING,"h":F}
  ```

- `advisor`, only when a loaded advisor reports real activation taps. No event is emitted otherwise.

  ```text
  {"gate_threshold":F,"winner":N,
   "layers":[{"name":"input|fc1|fc2|heads","size":N},...],
   "edges":[{"from":STRING,"to":STRING,"rows":N,"cols":N,"weights":ARRAY},...],
   "head_labels":ARRAY,
   "scale":{"input":F,"fc1":F,"fc2":F,"heads":F,"contribution":F},
   "frames":[{"candidate":N,"recommended":BOOL,"gate_pass":BOOL,"score":F,
     "action":{"mesher":STRING,"h":F,"order":N,"adapt":N},
     "input":ARRAY,"fc1":ARRAY,"fc2":ARRAY,"heads":ARRAY},...]}
  ```

  The four layer scales and connection-contribution scale are p98 magnitudes pooled over all real forward-pass frames. `winner` indexes `head_labels` for the selected mesher-policy head, or is `-1` when that exact deployed label is absent. The recommended candidate is identified separately by `frames[].recommended`.

- `mesh`

  ```text
  {"stage":STRING,"index":N,"pass":N,"n_elems":N,"n_nodes":N,
   "emitted_elems":N,
   "cells":{"n_verts":N,"positions_b64":B64,"centroids_b64":B64,
     "index_b64":B64,"color_b64":B64,"edges_b64":B64,"n_edge_verts":N}}
  ```

  Stage names are `lattice`, `expand`, `snap`, `peel`, `reproject`, `smooth`, `resnap`, `pin`, `fill`, and `ship`; meshers expose only stages they actually execute. `n_elems` is the true full mesh count. If the boundary exceeds the stream vertex budget, the server uniformly drops whole owning elements and `emitted_elems` reports the real retained count. The reveal index remains normalized against the true element index/count.

- `progress`

  ```text
  {"phase":STRING,"phase_frac":F,"elapsed_ms":N,"pass":N,"pass_count":N,
   "cg_iter":N,"cg_resid":F,"n_elems":N,"n_nodes":N}
  ```

- `pass`

  ```text
  {"pass":N,"n_elems":N,"n_nodes":N,"dof":N,"global_eta":F,"eta_p90":F,
   "mesh_ms":F,"solve_ms":F,"cg_iters":N,"solve_method":STRING}
  ```

- `result`, for a successful solve

  ```text
  {"n_nodes":N,"n_elems":N,"dof":N,
   "max_von_mises":F,"max_displacement":F,"global_eta":F,
   "mesh_note":STRING,"solver_note":STRING,
   "surface":{"n_verts":N,"positions_b64":B64,"normals_b64":B64,
     "disp_b64":B64,"von_mises_b64":B64,"u_mag_b64":B64,
     "eta_b64":B64,"edges_b64":B64,"n_edge_verts":N}}
  ```

  Surface samples use the same isoparametric boundary tessellation and nodal interpolation weights as the desktop viewport.

- `note`

  ```json
  {"text":"solver provenance or mesh note"}
  ```

- `done`, after all other events

  ```text
  {"state":"done|failed|cancelled","message":STRING,"elapsed_ms":N}
  ```

  `state` is `done`, `failed`, or `cancelled`.

### `DELETE /api/jobs/{id}`

Requests cooperative cancellation:

```json
{"ok":true,"cancelled":true}
```

The event stream subsequently ends with a `done` event whose state is `cancelled` once the pipeline reaches a cancellation checkpoint.

### `GET /api/jobs/{id}/result.vtu`

Downloads the completed solve as `application/octet-stream`. The server creates the VTU on first request with displacement, von Mises, displacement magnitude, nodal eta, and element eta fields. It returns 404 until the result is ready.

### Static files

`GET /` serves `index.html`. Other non-API paths are resolved under the configured web root, with the application index as the client-route fallback. Supported MIME types include HTML, CSS, JavaScript, SVG, PNG, TTF, WOFF2, and JSON. `HEAD` follows the corresponding `GET` route but omits the response body. Every response uses `Connection: close`.
