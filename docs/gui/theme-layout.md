# GUI theme & layout

Presentation lives only in `apps/gui/`. Tokens: `theme.hpp` / `theme.cpp`.
Widgets: `widgets.hpp` / `widgets.cpp`. Viewport: `viewport.*`.
Shared FEA colormap: `colormap.hpp`. PNG capture: `png_writer.hpp`.

## Rules
1. **No raw colors** in widgets/viewport/main — use palette tokens.
2. **Theme switch** goes through one apply function in `theme.cpp`.
3. **Layout** uses fixed constrained panels; prefer widget helpers for
   spacing/centering over one-off magic numbers. Group boxes auto-size to
   content (no hardcoded content heights). Selectors wrap when options would
   overflow. Button labels are clipped/centered inside the control bounds.
4. **No physics** in `apps/gui` — call `pipeline` / libraries.

## Layout

One host window, fixed four-column workspace, docking deliberately disabled
(`kPanelFlags`), so panels cannot be dragged out, collapsed, or lost:

```
menu bar (file | view)
Test Lab | Sim Setup | 3D viewport | Results
status strip
```

Column splitters resize the three side panels within clamps that keep the
viewport at a usable width; the status strip carries all frame state.

## Themes

Three palettes, all selectable from **view → theme**. `ThemeId` values are
persisted in `GuiSettings`, so new themes are **appended** and existing values
are never renumbered.

| id | name | chrome | viewport |
| -- | ---- | ------ | -------- |
| 0 | Interwebz | plum / rose | light studio gradient |
| 1 | Slate | neutral blue-grey | light studio gradient |
| 2 | **Studio** (default) | graphite / cyan | dark gradient |

### Studio tokens

Studio is the default theme and the palette every showcase render is captured
in. Chrome:

| token | hex | note |
| ----- | --- | ---- |
| `window_bg` | `#0E1116` | app chrome behind the panels |
| `panel_bg` | `#161B22` | panel / child background |
| `header_bg` | `#1C2330` | menu bar, group-box header strip |
| `border` | `#2A3240` | 1 px borders and separators |
| `text` | `#E6EAF0` | primary label text |
| `text_dim` | `#8A93A3` | secondary labels, control hints |
| `accent` | `#4CC2FF` | cyan — active state, gradient fills, group-box edge |
| `accent_dim` | `#2A6E96` | slider grab, low-emphasis accent |

Viewport and overlays:

| token | hex | note |
| ----- | --- | ---- |
| `viewport_top` | `#1B2028` | background gradient, top |
| `viewport_mid` | `#141922` | background gradient, middle |
| `viewport_bottom` | `#0F131A` | background gradient, bottom |
| `part_default` | `#8B95A5` | unassigned CAD face |
| `sim_fixture` | `#2ECC71` @ .65 | fixed region overlay |
| `sim_load` | `#F0433A` @ .65 | loaded region overlay |
| `status_ok` | `#2DD4BF` | ok / success text |
| `status_warn` | `#F5C542` | warning text |
| `status_err` | `#F5876C` | error text |

Studio style metrics: rounding `4.0`, `FramePadding {9, 6}`,
`ItemSpacing {8, 7}`, `ScrollbarSize 13`, border `1 px`. Interwebz and Slate
keep the tighter original metrics (rounding `2.0`, `FramePadding {8, 5}`).

Group-box headers carry a 2 px `accent` rule down their left edge
(`widgets.cpp`, `end_group_box`). It is palette-driven, so it reads correctly in
all three themes.

The stress colormap (`colormap.hpp`, blue→cyan→green→yellow→red) is **not** a
theme token — it is the same ramp in every theme, shared by the viewport's
per-vertex result colors and the results colorbar so the legend can never drift
from the render.

### Theme swap and baked colors

Setup-mode overlay colors are baked into GL vertex buffers by
`Viewport::update_overlays`, and result colors are baked by
`Viewport::bake_result`. Switching themes therefore has to invalidate them: the
view-menu handler calls `Viewport::invalidate_colors()` (drops the result bake
signature) and sets `overlays_dirty` so the fixture/load overlays are rebuilt
from the new palette on the next frame.

## Adding a colorscheme
1. Add a `make_*_palette()` in `theme.cpp` defining **every** `Palette` field.
2. Append a `ThemeId` value (never renumber) and wire it into `apply_theme`.
3. Add the menu entry in `draw_frame`, routed through the same `pick_theme`
   helper so the baked-color invalidation runs.
4. Keep contrast readable for stress heatmaps on the viewport.

## Fonts

`run()` loads a proportional UI face at 16 px, first match wins:

1. `$POLYMESH_GUI_FONT` — absolute path to a `.ttf`, if set.
2. `/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf`
3. `/usr/share/fonts/google-noto/NotoSans-Regular.ttf`
4. `/usr/share/fonts/dejavu/DejaVuSans.ttf`

Each candidate is existence-checked before `AddFontFromFileTTF`, because that
call asserts on a missing file in debug builds. If none exist, the app keeps
Dear ImGui's stock bitmap font and runs unchanged — the status strip height
adapts to the loaded line height either way.

## Screenshots

The app can write a PNG of its own window with no new dependencies:
`png_writer.hpp` emits signature + IHDR + IDAT + IEND, where IDAT is a zlib
stream built from DEFLATE *stored* (uncompressed) blocks. Capture reads the
default framebuffer with `glReadPixels` after every draw call of the frame and
before `glfwSwapBuffers`.

| trigger | destination |
| ------- | ----------- |
| `F12` | `./polymesh_shot_<UTC>.png` in the process CWD, e.g. `polymesh_shot_20260808T141530Z.png` |
| **file → save screenshot (F12)** | same as `F12`; capture is deferred one frame so the open menu popup stays out of the shot |
| `POLYMESH_GUI_SHOT=/abs/path.png` | that exact path, rewritten at most once per second while the variable is set |

`F12` and the menu item post a transient message into the status strip
(`status_ok` on success, `status_err` on failure). The environment path is the
headless route: run the app under Xvfb, let it write a frame, then kill it.

```sh
POLYMESH_GUI_SHOT=/abs/path/gui_studio.png \
  xvfb-run -a -s "-screen 0 1600x1000x24" ./build/apps/gui/polymesh-gui part.step
```

The app never writes into `docs/` on its own — the caller chooses the path.
