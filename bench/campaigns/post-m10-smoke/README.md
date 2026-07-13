# post-m10-smoke

Quick health pack after M10 + cylinder load-face normal filter (not a frozen
baseline — see `varyhedron-baseline-m9` for packing deltas).

- Parts: cylinder, plate_hole
- Meshers: varyhedron, hybrid_zoo
- Tier: h_scale=5.0
- Expected: 4/4 `ok`, `health.ok` and `load_area_ok` true

Run:

```bash
./build/apps/testlab/polymesh_testlab run bench/campaigns/post-m10-smoke
```
