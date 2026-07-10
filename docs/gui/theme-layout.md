# GUI theme & layout

Presentation lives only in `apps/gui/`. Tokens: `theme.hpp` / `theme.cpp`.
Widgets: `widgets.hpp` / `widgets.cpp` (Interwebz-style). Viewport: `viewport.*`.

## Rules
1. **No raw colors** in widgets/viewport/main — use palette tokens.
2. **Theme switch** goes through one apply function in `theme.cpp`.
3. **Layout** uses fixed constrained panels; prefer widget helpers for
   spacing/centering over one-off magic numbers.
4. **No physics** in `apps/gui` — call `pipeline` / libraries.

## Adding a colorscheme
1. Add a named palette struct/values in `theme.hpp`.
2. Wire selection in the theme apply path.
3. Keep contrast readable for stress heatmaps on the viewport.
