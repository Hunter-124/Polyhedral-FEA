# bench/results/archive

Files here are deliberately OUTSIDE the reach of `bench/competitive/render_scoreboard.py`, which globs `bench/results/*.json`
non-recursively and plots every schema-valid row it finds.

They are not results:

- `gmsh-peer.pre-enginefix.json` — the peer matrix as it stood before the feature-aware classification,
  load-area rescale, load-rule asymmetry and CG changes. It is a schema-VALID 144-row file, so while it sat in
  `bench/results/` every peer case was plotted twice, once with current numbers and once with pre-fix ones.
  A duplicate that validates is invisible to the loader's non-row check, which is why it slipped through.
- `gmsh-peer-expected.json` — those same 144 rows re-scored against the current references. An EXPECTATION
  used to attribute movement in a re-run (gmsh rows are the control: their mesh is unchanged, so probe movement
  there isolates the solver, while native/graded movement isolates the mesher). Never a measurement.

Rule of thumb: `bench/results/*.json` is for rows produced by the current code. Snapshots, baselines and
expectations belong here.
