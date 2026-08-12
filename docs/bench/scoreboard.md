# Benchmark scoreboard

_Generated 2026-08-12T03:04:35Z from `bench/results/*.json` via `bench/competitive/render_scoreboard.py`. Schema: `bench/competitive/schema.json`._

Primary DOF-reduction baseline is PolyMesh's frozen P1 uniform path (ADR-0005). Peer solvers are audit cross-checks.

## All runs

| Solver | Version | Case | DOFs | mesh s | solve s | total s | Accuracy | Value | Label | Timestamp |
|---|---|---|---:|---:|---:|---:|---|---:|---|---|
| PolyMesh | 0.1.0 | kirsch-plate | 1509 | — | — | 0.39 | scf_rel_err_pct | 1.87 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | lame-cylinder | 1257 | — | — | 0.32 | u_r_rel_err_pct | 0.0068 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | lame-cylinder | 1257 | — | — | 0.32 | hoop_rel_err_pct | 1.36 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | timoshenko-cantilever | 1440 | — | — | 0.45 | tip_rel_err_pct | 1.5 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | l-domain-d6-baseline | 6384 | 0.01322 | 2.749 | 2.762 | energy_deficit_pct | 0.08537 | `d6-tier3` | 2026-07-10T10:20:00Z |
| PolyMesh | 0.1.0 | l-domain-d6-graded | 1248 | 0.001028 | 0.2257 | 0.2267 | energy_deficit_pct | 0.08881 | `d6-tier3` | 2026-07-10T10:20:00Z |
| PolyMesh | 0.1.0 | l-domain-d6-ratio | 1248 | — | — | 0.2267 | dof_ratio_uniform_over_graded | 5.115 | `d6-tier3` | 2026-07-10T10:20:00Z |
| PolyMesh | 0.1.0 | l-domain-d6-ratio | 1248 | — | — | 0.2267 | time_ratio_uniform_over_graded | 12.18 | `d6-tier3` | 2026-07-10T10:20:00Z |
| calculix | 2.23 | cantilever | 48 | — | 0.1084 | 0.1084 | tip_deflection_rel_err | 0.7219 | `calculix-cantilever-hex-4x1x1` | 2026-08-11T18:47:01.551398+00:00 |
| polymesh-native | 92455f9 | cantilever | 48 | — | 0.05792 | 0.05792 | tip_deflection_rel_err | 0.7219 | `polymesh-native-cantilever-hex-4x1x1` | 2026-08-11T18:47:01.551398+00:00 |
| calculix | 2.23 | cantilever | 216 | — | 0.1484 | 0.1484 | tip_deflection_rel_err | 0.402 | `calculix-cantilever-hex-8x2x2` | 2026-08-11T18:47:01.783438+00:00 |
| polymesh-native | 92455f9 | cantilever | 216 | — | 0.06026 | 0.06026 | tip_deflection_rel_err | 0.402 | `polymesh-native-cantilever-hex-8x2x2` | 2026-08-11T18:47:01.783438+00:00 |
| calculix | 2.23 | cantilever | 1200 | — | 0.3103 | 0.3103 | tip_deflection_rel_err | 0.1513 | `calculix-cantilever-hex-16x4x4` | 2026-08-11T18:47:02.304560+00:00 |
| polymesh-native | 92455f9 | cantilever | 1200 | — | 0.1732 | 0.1732 | tip_deflection_rel_err | 0.1513 | `polymesh-native-cantilever-hex-16x4x4` | 2026-08-11T18:47:02.304560+00:00 |
| calculix | 2.23 | cantilever | 7776 | — | 1.251 | 1.251 | tip_deflection_rel_err | 0.0488 | `calculix-cantilever-hex-32x8x8` | 2026-08-11T18:47:05.851931+00:00 |
| polymesh-native | 92455f9 | cantilever | 7776 | — | 2.212 | 2.212 | tip_deflection_rel_err | 0.0488 | `polymesh-native-cantilever-hex-32x8x8` | 2026-08-11T18:47:05.851931+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s0_c0 | 720 | 0.2144 | 0.1417 | 0.3561 | scf_rel_err | 0.3578 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:54:55.117224+00:00 |
| polymesh-native | 08f9f55 | box_hole_s0_c0 | 621 | — | — | 0.615 | scf_rel_err | — | `gmsh-peer-h0.20-p1` | 2026-08-12T02:54:55.117224+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s0_c0 | 1161 | — | — | 1.083 | scf_rel_err | — | `gmsh-peer-h0.20-p1` | 2026-08-12T02:54:55.117224+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s0_c0 | 4206 | 0.2689 | 0.3212 | 0.5901 | scf_rel_err | 0.25 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:54:57.199418+00:00 |
| polymesh-native | 08f9f55 | box_hole_s0_c0 | 2145 | — | — | 1.048 | scf_rel_err | — | `gmsh-peer-h0.20-p2` | 2026-08-12T02:54:57.199418+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s0_c0 | 7272 | — | — | 2.29 | scf_rel_err | — | `gmsh-peer-h0.20-p2` | 2026-08-12T02:54:57.199418+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s0_c0 | 720 | 0.2013 | 0.09743 | 0.2987 | scf_rel_err | 0.3578 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:55:01.204724+00:00 |
| polymesh-native | 08f9f55 | box_hole_s0_c0 | 1296 | — | — | 1.361 | scf_rel_err | 0.6632 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:55:01.204724+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s0_c0 | 3294 | — | — | 2.149 | scf_rel_err | 0.6634 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:55:01.204724+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s0_c0 | 4206 | 0.3354 | 0.2622 | 0.5976 | scf_rel_err | 0.25 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:55:05.069182+00:00 |
| polymesh-native | 08f9f55 | box_hole_s0_c0 | 4557 | — | — | 1.658 | scf_rel_err | 0.6592 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:55:05.069182+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s0_c0 | 21003 | — | — | 4.436 | scf_rel_err | 0.6494 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:55:05.069182+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s0_c0 | 978 | 0.2332 | 0.1029 | 0.3361 | scf_rel_err | 0.364 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:55:11.901362+00:00 |
| polymesh-native | 08f9f55 | box_hole_s0_c0 | 2808 | — | — | 3.053 | scf_rel_err | 0.6635 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:55:11.901362+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s0_c0 | 7581 | — | — | 5.625 | scf_rel_err | 0.09069 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:55:11.901362+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s0_c0 | 5715 | 0.3831 | 0.3413 | 0.7244 | scf_rel_err | 0.2737 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:55:20.999263+00:00 |
| polymesh-native | 08f9f55 | box_hole_s0_c0 | 10014 | — | — | 5.023 | scf_rel_err | 0.6618 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:55:20.999263+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s0_c0 | 49482 | — | — | 16.65 | scf_rel_err | 0.1157 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:55:20.999263+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s1_c0 | 657 | 0.2121 | 0.06743 | 0.2795 | scf_rel_err | 0.2818 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:55:43.639729+00:00 |
| polymesh-native | 08f9f55 | box_hole_s1_c0 | 621 | — | — | 0.4927 | scf_rel_err | — | `gmsh-peer-h0.20-p1` | 2026-08-12T02:55:43.639729+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s1_c0 | 1272 | — | — | 1.043 | scf_rel_err | — | `gmsh-peer-h0.20-p1` | 2026-08-12T02:55:43.639729+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s1_c0 | 3828 | 0.2858 | 0.2198 | 0.5056 | scf_rel_err | 0.2403 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:55:45.481791+00:00 |
| polymesh-native | 08f9f55 | box_hole_s1_c0 | 2145 | — | — | 0.9616 | scf_rel_err | — | `gmsh-peer-h0.20-p2` | 2026-08-12T02:55:45.481791+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s1_c0 | 7992 | — | — | 1.778 | scf_rel_err | — | `gmsh-peer-h0.20-p2` | 2026-08-12T02:55:45.481791+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s1_c0 | 657 | 0.1972 | 0.06732 | 0.2645 | scf_rel_err | 0.2818 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:55:48.815956+00:00 |
| polymesh-native | 08f9f55 | box_hole_s1_c0 | 1296 | — | — | 1.011 | scf_rel_err | 0.6624 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:55:48.815956+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s1_c0 | 3396 | — | — | 2.352 | scf_rel_err | 0.5376 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:55:48.815956+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s1_c0 | 3828 | 0.2793 | 0.1922 | 0.4715 | scf_rel_err | 0.2403 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:55:52.485521+00:00 |
| polymesh-native | 08f9f55 | box_hole_s1_c0 | 4557 | — | — | 1.892 | scf_rel_err | 0.6571 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:55:52.485521+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s1_c0 | 21828 | — | — | 4.832 | scf_rel_err | 0.5034 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:55:52.485521+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s1_c0 | 906 | 0.1951 | 0.1067 | 0.3018 | scf_rel_err | 0.2972 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:55:59.817269+00:00 |
| polymesh-native | 08f9f55 | box_hole_s1_c0 | 2547 | — | — | 2.075 | scf_rel_err | 0.7299 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:55:59.817269+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s1_c0 | 5232 | — | — | 3.603 | scf_rel_err | 0.4845 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:55:59.817269+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s1_c0 | 5304 | 0.3072 | 0.2694 | 0.5767 | scf_rel_err | 0.2463 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:56:05.868364+00:00 |
| polymesh-native | 08f9f55 | box_hole_s1_c0 | 9078 | — | — | 4.267 | scf_rel_err | 0.5984 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:56:05.868364+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s1_c0 | 33282 | — | — | 7.863 | scf_rel_err | 0.5186 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:56:05.868364+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s2_c0 | 738 | 0.2006 | 0.08974 | 0.2904 | scf_rel_err | 0.3598 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:56:18.828120+00:00 |
| polymesh-native | 08f9f55 | box_hole_s2_c0 | 621 | — | — | 0.5614 | scf_rel_err | — | `gmsh-peer-h0.20-p1` | 2026-08-12T02:56:18.828120+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s2_c0 | 1293 | — | — | 0.9462 | scf_rel_err | — | `gmsh-peer-h0.20-p1` | 2026-08-12T02:56:18.828120+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s2_c0 | 4314 | 0.2987 | 0.3183 | 0.617 | scf_rel_err | 0.2604 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:56:20.659047+00:00 |
| polymesh-native | 08f9f55 | box_hole_s2_c0 | 2145 | — | — | 0.9256 | scf_rel_err | — | `gmsh-peer-h0.20-p2` | 2026-08-12T02:56:20.659047+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s2_c0 | 8118 | — | — | 1.64 | scf_rel_err | — | `gmsh-peer-h0.20-p2` | 2026-08-12T02:56:20.659047+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s2_c0 | 738 | 0.1963 | 0.0839 | 0.2801 | scf_rel_err | 0.3598 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:56:23.919374+00:00 |
| polymesh-native | 08f9f55 | box_hole_s2_c0 | 1296 | — | — | 1.288 | scf_rel_err | 0.6629 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:56:23.919374+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s2_c0 | 3291 | — | — | 2.167 | scf_rel_err | 0.663 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:56:23.919374+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s2_c0 | 4314 | 0.3019 | 0.3352 | 0.6371 | scf_rel_err | 0.2604 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:56:27.700700+00:00 |
| polymesh-native | 08f9f55 | box_hole_s2_c0 | 4557 | — | — | 1.679 | scf_rel_err | 0.6583 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:56:27.700700+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s2_c0 | 21000 | — | — | 4.12 | scf_rel_err | 0.6268 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:56:27.700700+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s2_c0 | 1026 | 0.1971 | 0.09933 | 0.2964 | scf_rel_err | 0.3575 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:56:34.255022+00:00 |
| polymesh-native | 08f9f55 | box_hole_s2_c0 | 2808 | — | — | 3.168 | scf_rel_err | 0.6632 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:56:34.255022+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s2_c0 | 7725 | — | — | 5.281 | scf_rel_err | 0.08303 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:56:34.255022+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s2_c0 | 6000 | 0.3362 | 0.3495 | 0.6856 | scf_rel_err | 0.2784 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:56:43.069269+00:00 |
| polymesh-native | 08f9f55 | box_hole_s2_c0 | 10014 | — | — | 4.831 | scf_rel_err | 0.6612 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:56:43.069269+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s2_c0 | 50538 | — | — | 92 | scf_rel_err | 0.007156 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:56:43.069269+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s3_c0 | 738 | 0.2425 | 0.1916 | 0.4342 | scf_rel_err | 0.3468 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:58:20.873900+00:00 |
| polymesh-native | 08f9f55 | box_hole_s3_c0 | 621 | — | — | 0.6746 | scf_rel_err | — | `gmsh-peer-h0.20-p1` | 2026-08-12T02:58:20.873900+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s3_c0 | 1203 | — | — | 1.248 | scf_rel_err | — | `gmsh-peer-h0.20-p1` | 2026-08-12T02:58:20.873900+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s3_c0 | 4305 | 0.3058 | 0.3289 | 0.6347 | scf_rel_err | 0.2981 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:58:23.276872+00:00 |
| polymesh-native | 08f9f55 | box_hole_s3_c0 | 2145 | — | — | 1.074 | scf_rel_err | — | `gmsh-peer-h0.20-p2` | 2026-08-12T02:58:23.276872+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s3_c0 | 7539 | — | — | 1.596 | scf_rel_err | — | `gmsh-peer-h0.20-p2` | 2026-08-12T02:58:23.276872+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s3_c0 | 738 | 0.1898 | 0.07284 | 0.2627 | scf_rel_err | 0.3468 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:58:26.650921+00:00 |
| polymesh-native | 08f9f55 | box_hole_s3_c0 | 1296 | — | — | 1.165 | scf_rel_err | 0.6625 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:58:26.650921+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s3_c0 | 3255 | — | — | 2.211 | scf_rel_err | 0.6626 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:58:26.650921+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s3_c0 | 4305 | 0.325 | 0.2646 | 0.5896 | scf_rel_err | 0.2981 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:58:30.330138+00:00 |
| polymesh-native | 08f9f55 | box_hole_s3_c0 | 4557 | — | — | 1.854 | scf_rel_err | 0.6573 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:58:30.330138+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s3_c0 | 20721 | — | — | 4.727 | scf_rel_err | 0.645 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:58:30.330138+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s3_c0 | 1041 | 0.2499 | 0.09791 | 0.3478 | scf_rel_err | 0.3665 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:58:37.642012+00:00 |
| polymesh-native | 08f9f55 | box_hole_s3_c0 | 2808 | — | — | 2.618 | scf_rel_err | 0.6628 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:58:37.642012+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s3_c0 | 7314 | — | — | 5.201 | scf_rel_err | 0.008669 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:58:37.642012+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | box_hole_s3_c0 | 6099 | 0.3622 | 0.32 | 0.6822 | scf_rel_err | 0.2883 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:58:45.875301+00:00 |
| polymesh-native | 08f9f55 | box_hole_s3_c0 | 10014 | — | — | 5.254 | scf_rel_err | 0.6605 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:58:45.875301+00:00 |
| polymesh-native-graded | 08f9f55 | box_hole_s3_c0 | 47877 | — | — | 15.1 | scf_rel_err | 0.3131 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:58:45.875301+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s0_c1 | 288 | 0.1642 | 0.04612 | 0.2103 | tip_deflection_rel_err | 0.2486 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:59:07.189863+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s0_c1 | 270 | — | — | 0.2701 | tip_deflection_rel_err | 0.3665 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:59:07.189863+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s0_c1 | 1695 | — | — | 0.6791 | tip_deflection_rel_err | 0.1728 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:59:07.189863+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s0_c1 | 1467 | 17.97 | 0.0775 | 18.05 | tip_deflection_rel_err | 0.7181 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:59:08.385813+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s0_c1 | 849 | — | — | 0.274 | tip_deflection_rel_err | 0.06485 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:59:08.385813+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s0_c1 | 10887 | — | — | 1.29 | tip_deflection_rel_err | 0.05039 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:59:08.385813+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s0_c1 | 288 | 0.1463 | 0.05076 | 0.1971 | tip_deflection_rel_err | 0.2486 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:59:28.074717+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s0_c1 | 486 | — | — | 0.294 | tip_deflection_rel_err | 0.1486 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:59:28.074717+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s0_c1 | 1071 | — | — | 0.6127 | tip_deflection_rel_err | 0.2361 | `gmsh-peer-h0.12-p1` | 2026-08-12T02:59:28.074717+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s0_c1 | 1467 | 19.79 | 0.1009 | 19.89 | tip_deflection_rel_err | 0.7381 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:59:29.210119+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s0_c1 | 1503 | — | — | 0.4793 | tip_deflection_rel_err | 0.06578 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:59:29.210119+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s0_c1 | 6630 | — | — | 1.066 | tip_deflection_rel_err | 0.1245 | `gmsh-peer-h0.12-p2` | 2026-08-12T02:59:29.210119+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s0_c1 | 393 | 0.17 | 0.0646 | 0.2346 | tip_deflection_rel_err | 0.2795 | `gmsh-peer-h0.08-p1` | 2026-08-12T02:59:50.712970+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s0_c1 | — | — | — | 0.5594 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p1` | 2026-08-12T02:59:50.712970+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s0_c1 | — | — | — | 1.412 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p1` | 2026-08-12T02:59:50.712970+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s0_c1 | 2088 | 3.025 | 0.1106 | 3.135 | tip_deflection_rel_err | 0.06202 | `gmsh-peer-h0.08-p2` | 2026-08-12T02:59:52.931552+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s0_c1 | — | — | — | 0.4053 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p2` | 2026-08-12T02:59:52.931552+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s0_c1 | — | — | — | 1.247 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p2` | 2026-08-12T02:59:52.931552+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s1_c1 | 300 | 0.1773 | 0.1473 | 0.3246 | tip_deflection_rel_err | 0.211 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:59:57.737375+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s1_c1 | 270 | — | — | 0.2815 | tip_deflection_rel_err | 0.2897 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:59:57.737375+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s1_c1 | 1368 | — | — | 0.6109 | tip_deflection_rel_err | 0.157 | `gmsh-peer-h0.20-p1` | 2026-08-12T02:59:57.737375+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s1_c1 | 1551 | 16.93 | 0.1102 | 17.04 | tip_deflection_rel_err | 0.436 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:59:58.993751+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s1_c1 | 855 | — | — | 0.238 | tip_deflection_rel_err | 0.1071 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:59:58.993751+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s1_c1 | 8865 | — | — | 1.131 | tip_deflection_rel_err | 0.07704 | `gmsh-peer-h0.20-p2` | 2026-08-12T02:59:58.993751+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s1_c1 | 300 | 0.1478 | 0.04896 | 0.1968 | tip_deflection_rel_err | 0.211 | `gmsh-peer-h0.12-p1` | 2026-08-12T03:00:17.471469+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s1_c1 | 486 | — | — | 0.3257 | tip_deflection_rel_err | 0.07107 | `gmsh-peer-h0.12-p1` | 2026-08-12T03:00:17.471469+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s1_c1 | 1356 | — | — | 0.6303 | tip_deflection_rel_err | 0.1293 | `gmsh-peer-h0.12-p1` | 2026-08-12T03:00:17.471469+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s1_c1 | 1551 | 16.68 | 0.08154 | 16.76 | tip_deflection_rel_err | 0.441 | `gmsh-peer-h0.12-p2` | 2026-08-12T03:00:18.664926+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s1_c1 | 1491 | — | — | 0.5459 | tip_deflection_rel_err | 0.03108 | `gmsh-peer-h0.12-p2` | 2026-08-12T03:00:18.664926+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s1_c1 | 8328 | — | — | 1.06 | tip_deflection_rel_err | 0.1649 | `gmsh-peer-h0.12-p2` | 2026-08-12T03:00:18.664926+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s1_c1 | 342 | 0.1537 | 0.1587 | 0.3124 | tip_deflection_rel_err | 0.3252 | `gmsh-peer-h0.08-p1` | 2026-08-12T03:00:37.082856+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s1_c1 | 1950 | — | — | 0.8109 | tip_deflection_rel_err | 0.08242 | `gmsh-peer-h0.08-p1` | 2026-08-12T03:00:37.082856+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s1_c1 | 9243 | — | — | 2.658 | tip_deflection_rel_err | 0.0971 | `gmsh-peer-h0.08-p1` | 2026-08-12T03:00:37.082856+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s1_c1 | 1812 | 5.108 | 0.1016 | 5.209 | tip_deflection_rel_err | 0.4403 | `gmsh-peer-h0.08-p2` | 2026-08-12T03:00:40.937356+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s1_c1 | 6951 | — | — | 1.695 | tip_deflection_rel_err | 0.02132 | `gmsh-peer-h0.08-p2` | 2026-08-12T03:00:40.937356+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s1_c1 | 64305 | — | — | 80.63 | tip_deflection_rel_err | 0.02889 | `gmsh-peer-h0.08-p2` | 2026-08-12T03:00:40.937356+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s2_c1 | 291 | 0.1802 | 0.06418 | 0.2443 | tip_deflection_rel_err | 0.2276 | `gmsh-peer-h0.20-p1` | 2026-08-12T03:02:08.822462+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s2_c1 | 270 | — | — | 0.2432 | tip_deflection_rel_err | 0.388 | `gmsh-peer-h0.20-p1` | 2026-08-12T03:02:08.822462+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s2_c1 | 1188 | — | — | 0.5798 | tip_deflection_rel_err | 0.2071 | `gmsh-peer-h0.20-p1` | 2026-08-12T03:02:08.822462+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s2_c1 | 1482 | 5.091 | 0.08095 | 5.172 | tip_deflection_rel_err | 0.7718 | `gmsh-peer-h0.20-p2` | 2026-08-12T03:02:09.923967+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s2_c1 | 855 | — | — | 0.3105 | tip_deflection_rel_err | 0.07208 | `gmsh-peer-h0.20-p2` | 2026-08-12T03:02:09.923967+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s2_c1 | 7491 | — | — | 0.9938 | tip_deflection_rel_err | 0.03215 | `gmsh-peer-h0.20-p2` | 2026-08-12T03:02:09.923967+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s2_c1 | 291 | 0.1627 | 0.04629 | 0.209 | tip_deflection_rel_err | 0.2276 | `gmsh-peer-h0.12-p1` | 2026-08-12T03:02:16.447443+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s2_c1 | 486 | — | — | 0.3497 | tip_deflection_rel_err | 0.1373 | `gmsh-peer-h0.12-p1` | 2026-08-12T03:02:16.447443+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s2_c1 | 1131 | — | — | 0.6037 | tip_deflection_rel_err | 0.2061 | `gmsh-peer-h0.12-p1` | 2026-08-12T03:02:16.447443+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s2_c1 | 1482 | 4.272 | 0.1223 | 4.395 | tip_deflection_rel_err | 0.7664 | `gmsh-peer-h0.12-p2` | 2026-08-12T03:02:17.644679+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s2_c1 | 1503 | — | — | 0.387 | tip_deflection_rel_err | 0.08822 | `gmsh-peer-h0.12-p2` | 2026-08-12T03:02:17.644679+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s2_c1 | 6963 | — | — | 1.038 | tip_deflection_rel_err | 0.1244 | `gmsh-peer-h0.12-p2` | 2026-08-12T03:02:17.644679+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s2_c1 | 336 | 0.2074 | 0.06166 | 0.2691 | tip_deflection_rel_err | 0.288 | `gmsh-peer-h0.08-p1` | 2026-08-12T03:02:23.543383+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s2_c1 | — | — | — | 0.4359 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p1` | 2026-08-12T03:02:23.543383+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s2_c1 | — | — | — | 1.041 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p1` | 2026-08-12T03:02:23.543383+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s2_c1 | 1779 | 46.57 | 0.2025 | 46.77 | tip_deflection_rel_err | 0.3952 | `gmsh-peer-h0.08-p2` | 2026-08-12T03:02:25.303431+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s2_c1 | — | — | — | 0.3604 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p2` | 2026-08-12T03:02:25.303431+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s2_c1 | — | — | — | 0.9265 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p2` | 2026-08-12T03:02:25.303431+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s3_c1 | 279 | 0.1559 | 0.05107 | 0.207 | tip_deflection_rel_err | 0.2141 | `gmsh-peer-h0.20-p1` | 2026-08-12T03:03:13.380892+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s3_c1 | 270 | — | — | 0.1962 | tip_deflection_rel_err | 0.3113 | `gmsh-peer-h0.20-p1` | 2026-08-12T03:03:13.380892+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s3_c1 | 1170 | — | — | 0.449 | tip_deflection_rel_err | 0.1898 | `gmsh-peer-h0.20-p1` | 2026-08-12T03:03:13.380892+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s3_c1 | 1431 | 15.24 | 0.09838 | 15.33 | tip_deflection_rel_err | 0.3339 | `gmsh-peer-h0.20-p2` | 2026-08-12T03:03:14.265781+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s3_c1 | 855 | — | — | 0.2599 | tip_deflection_rel_err | 0.06477 | `gmsh-peer-h0.20-p2` | 2026-08-12T03:03:14.265781+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s3_c1 | 7374 | — | — | 0.8698 | tip_deflection_rel_err | 0.03857 | `gmsh-peer-h0.20-p2` | 2026-08-12T03:03:14.265781+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s3_c1 | 279 | 0.1473 | 0.03906 | 0.1864 | tip_deflection_rel_err | 0.2141 | `gmsh-peer-h0.12-p1` | 2026-08-12T03:03:30.779108+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s3_c1 | 486 | — | — | 0.3847 | tip_deflection_rel_err | 0.07121 | `gmsh-peer-h0.12-p1` | 2026-08-12T03:03:30.779108+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s3_c1 | 1053 | — | — | 0.5206 | tip_deflection_rel_err | 0.1656 | `gmsh-peer-h0.12-p1` | 2026-08-12T03:03:30.779108+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s3_c1 | 1431 | 14.94 | 0.1736 | 15.12 | tip_deflection_rel_err | 0.3231 | `gmsh-peer-h0.12-p2` | 2026-08-12T03:03:31.901250+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s3_c1 | 1515 | — | — | 0.3529 | tip_deflection_rel_err | 0.08472 | `gmsh-peer-h0.12-p2` | 2026-08-12T03:03:31.901250+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s3_c1 | 6489 | — | — | 0.8116 | tip_deflection_rel_err | 0.1373 | `gmsh-peer-h0.12-p2` | 2026-08-12T03:03:31.901250+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s3_c1 | 384 | 0.1474 | 0.0491 | 0.1965 | tip_deflection_rel_err | 0.2665 | `gmsh-peer-h0.08-p1` | 2026-08-12T03:03:48.241192+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s3_c1 | — | — | — | 0.4296 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p1` | 2026-08-12T03:03:48.241192+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s3_c1 | — | — | — | 1.021 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p1` | 2026-08-12T03:03:48.241192+00:00 |
| gmsh-mesh+polymesh-solver | gmsh-4.13.1+polymesh-08f9f55 | stepped_shaft_s3_c1 | 2052 | 2.585 | 0.1975 | 2.782 | tip_deflection_rel_err | 0.004738 | `gmsh-peer-h0.08-p2` | 2026-08-12T03:03:49.901469+00:00 |
| polymesh-native | 08f9f55 | stepped_shaft_s3_c1 | — | — | — | 0.3583 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p2` | 2026-08-12T03:03:49.901469+00:00 |
| polymesh-native-graded | 08f9f55 | stepped_shaft_s3_c1 | — | — | — | 0.9489 | tip_deflection_rel_err | — | `gmsh-peer-h0.08-p2` | 2026-08-12T03:03:49.901469+00:00 |

## Accuracy vs labeled commits

ASCII sparkline scales within each case/metric series (height ∝ value). SVG polyline when ≥2 numeric points. Lower is better for `*_err_*` metrics.

### `box_hole_s0_c0` — `scf_rel_err`

- labels: `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2`
- solvers: gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded
- values: 0.3578, —, —, 0.25, —, —, 0.3578, 0.6632, 0.6634, 0.25, 0.6592, 0.6494, 0.364, 0.6635, 0.09069, 0.2737, 0.6618, 0.1157
- sparkline: `▄··▂··▄▇▇▂▇▇▄█▁▃▇▁`

<svg xmlns="http://www.w3.org/2000/svg" width="180" height="36" role="img" aria-label="box_hole_s0_c0 scf_rel_err"><polyline fill="none" stroke="#4a9" stroke-width="1.5" points="3.0,19.0 33.7,24.7 64.4,19.0 74.6,3.0 84.9,3.0 95.1,24.7 105.4,3.2 115.6,3.7 125.8,18.7 136.1,3.0 146.3,33.0 156.5,23.4 166.8,3.1 177.0,31.7"/></svg>

### `box_hole_s1_c0` — `scf_rel_err`

- labels: `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2`
- solvers: gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded
- values: 0.2818, —, —, 0.2403, —, —, 0.2818, 0.6624, 0.5376, 0.2403, 0.6571, 0.5034, 0.2972, 0.7299, 0.4845, 0.2463, 0.5984, 0.5186
- sparkline: `▁··▁··▁▇▅▁▆▄▁█▄▁▆▄`

<svg xmlns="http://www.w3.org/2000/svg" width="180" height="36" role="img" aria-label="box_hole_s1_c0 scf_rel_err"><polyline fill="none" stroke="#4a9" stroke-width="1.5" points="3.0,30.5 33.7,33.0 64.4,30.5 74.6,7.1 84.9,14.8 95.1,33.0 105.4,7.5 115.6,16.9 125.8,29.5 136.1,3.0 146.3,18.0 156.5,32.6 166.8,11.1 177.0,15.9"/></svg>

### `box_hole_s2_c0` — `scf_rel_err`

- labels: `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2`
- solvers: gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded
- values: 0.3598, —, —, 0.2604, —, —, 0.3598, 0.6629, 0.663, 0.2604, 0.6583, 0.6268, 0.3575, 0.6632, 0.08303, 0.2784, 0.6612, 0.007156
- sparkline: `▄··▃··▄▇▇▃▇▇▄█▁▃▇▁`

<svg xmlns="http://www.w3.org/2000/svg" width="180" height="36" role="img" aria-label="box_hole_s2_c0 scf_rel_err"><polyline fill="none" stroke="#4a9" stroke-width="1.5" points="3.0,16.9 33.7,21.4 64.4,16.9 74.6,3.0 84.9,3.0 95.1,21.4 105.4,3.2 115.6,4.7 125.8,17.0 136.1,3.0 146.3,29.5 156.5,20.6 166.8,3.1 177.0,33.0"/></svg>

### `box_hole_s3_c0` — `scf_rel_err`

- labels: `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2`
- solvers: gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded
- values: 0.3468, —, —, 0.2981, —, —, 0.3468, 0.6625, 0.6626, 0.2981, 0.6573, 0.645, 0.3665, 0.6628, 0.008669, 0.2883, 0.6605, 0.3131
- sparkline: `▄··▄··▄▇▇▄▇▇▄█▁▃▇▄`

<svg xmlns="http://www.w3.org/2000/svg" width="180" height="36" role="img" aria-label="box_hole_s3_c0 scf_rel_err"><polyline fill="none" stroke="#4a9" stroke-width="1.5" points="3.0,17.5 33.7,19.7 64.4,17.5 74.6,3.0 84.9,3.0 95.1,19.7 105.4,3.3 115.6,3.8 125.8,16.6 136.1,3.0 146.3,33.0 156.5,20.2 166.8,3.1 177.0,19.0"/></svg>

### `cantilever` — `tip_deflection_rel_err`

- labels: `calculix-cantilever-hex-4x1x1` → `polymesh-native-cantilever-hex-4x1x1` → `calculix-cantilever-hex-8x2x2` → `polymesh-native-cantilever-hex-8x2x2` → `calculix-cantilever-hex-16x4x4` → `polymesh-native-cantilever-hex-16x4x4` → `calculix-cantilever-hex-32x8x8` → `polymesh-native-cantilever-hex-32x8x8`
- solvers: calculix, polymesh-native, calculix, polymesh-native, calculix, polymesh-native, calculix, polymesh-native
- values: 0.7219, 0.7219, 0.402, 0.402, 0.1513, 0.1513, 0.0488, 0.0488
- sparkline: `█▇▄▄▂▂▁▁`

<svg xmlns="http://www.w3.org/2000/svg" width="180" height="36" role="img" aria-label="cantilever tip_deflection_rel_err"><polyline fill="none" stroke="#4a9" stroke-width="1.5" points="3.0,3.0 27.9,3.0 52.7,17.3 77.6,17.3 102.4,28.4 127.3,28.4 152.1,33.0 177.0,33.0"/></svg>

### `kirsch-plate` — `scf_rel_err_pct`

- labels: `gate1-p1`
- solvers: PolyMesh
- values: 1.87
- sparkline: `▁`

### `l-domain-d6-baseline` — `energy_deficit_pct`

- labels: `d6-tier3`
- solvers: PolyMesh
- values: 0.08537
- sparkline: `▁`

### `l-domain-d6-graded` — `energy_deficit_pct`

- labels: `d6-tier3`
- solvers: PolyMesh
- values: 0.08881
- sparkline: `▁`

### `l-domain-d6-ratio` — `dof_ratio_uniform_over_graded`

- labels: `d6-tier3`
- solvers: PolyMesh
- values: 5.115
- sparkline: `▁`

### `l-domain-d6-ratio` — `time_ratio_uniform_over_graded`

- labels: `d6-tier3`
- solvers: PolyMesh
- values: 12.18
- sparkline: `▁`

### `lame-cylinder` — `hoop_rel_err_pct`

- labels: `gate1-p1`
- solvers: PolyMesh
- values: 1.36
- sparkline: `▁`

### `lame-cylinder` — `u_r_rel_err_pct`

- labels: `gate1-p1`
- solvers: PolyMesh
- values: 0.0068
- sparkline: `▁`

### `stepped_shaft_s0_c1` — `tip_deflection_rel_err`

- labels: `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2`
- solvers: gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded
- values: 0.2486, 0.3665, 0.1728, 0.7181, 0.06485, 0.05039, 0.2486, 0.1486, 0.2361, 0.7381, 0.06578, 0.1245, 0.2795, —, —, 0.06202, —, —
- sparkline: `▃▄▂▇▁▁▃▁▂█▁▁▃··▁··`

<svg xmlns="http://www.w3.org/2000/svg" width="180" height="36" role="img" aria-label="stepped_shaft_s0_c1 tip_deflection_rel_err"><polyline fill="none" stroke="#4a9" stroke-width="1.5" points="3.0,24.4 14.6,19.2 26.2,27.7 37.8,3.9 49.4,32.4 61.0,33.0 72.6,24.4 84.2,28.7 95.8,24.9 107.4,3.0 119.0,32.3 130.6,29.8 142.2,23.0 177.0,32.5"/></svg>

### `stepped_shaft_s1_c1` — `tip_deflection_rel_err`

- labels: `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2`
- solvers: gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded
- values: 0.211, 0.2897, 0.157, 0.436, 0.1071, 0.07704, 0.211, 0.07107, 0.1293, 0.441, 0.03108, 0.1649, 0.3252, 0.08242, 0.0971, 0.4403, 0.02132, 0.02889
- sparkline: `▄▅▃▇▂▁▄▁▂█▁▃▆▂▂▇▁▁`

<svg xmlns="http://www.w3.org/2000/svg" width="180" height="36" role="img" aria-label="stepped_shaft_s1_c1 tip_deflection_rel_err"><polyline fill="none" stroke="#4a9" stroke-width="1.5" points="3.0,19.4 13.2,13.8 23.5,23.3 33.7,3.4 43.9,26.9 54.2,29.0 64.4,19.4 74.6,29.4 84.9,25.3 95.1,3.0 105.4,32.3 115.6,22.7 125.8,11.3 136.1,28.6 146.3,27.6 156.5,3.1 166.8,33.0 177.0,32.5"/></svg>

### `stepped_shaft_s2_c1` — `tip_deflection_rel_err`

- labels: `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2`
- solvers: gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded
- values: 0.2276, 0.388, 0.2071, 0.7718, 0.07208, 0.03215, 0.2276, 0.1373, 0.2061, 0.7664, 0.08822, 0.1244, 0.288, —, —, 0.3952, —, —
- sparkline: `▂▄▂█▁▁▂▁▂▇▁▁▃··▄··`

<svg xmlns="http://www.w3.org/2000/svg" width="180" height="36" role="img" aria-label="stepped_shaft_s2_c1 tip_deflection_rel_err"><polyline fill="none" stroke="#4a9" stroke-width="1.5" points="3.0,25.1 14.6,18.6 26.2,25.9 37.8,3.0 49.4,31.4 61.0,33.0 72.6,25.1 84.2,28.7 95.8,25.9 107.4,3.2 119.0,30.7 130.6,29.3 142.2,22.6 177.0,18.3"/></svg>

### `stepped_shaft_s3_c1` — `tip_deflection_rel_err`

- labels: `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p1` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.20-p2` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p1` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.12-p2` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p1` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2` → `gmsh-peer-h0.08-p2`
- solvers: gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded, gmsh-mesh+polymesh-solver, polymesh-native, polymesh-native-graded
- values: 0.2141, 0.3113, 0.1898, 0.3339, 0.06477, 0.03857, 0.2141, 0.07121, 0.1656, 0.3231, 0.08472, 0.1373, 0.2665, —, —, 0.004738, —, —
- sparkline: `▅▇▄█▂▁▅▂▄▇▂▃▆··▁··`

<svg xmlns="http://www.w3.org/2000/svg" width="180" height="36" role="img" aria-label="stepped_shaft_s3_c1 tip_deflection_rel_err"><polyline fill="none" stroke="#4a9" stroke-width="1.5" points="3.0,13.9 14.6,5.1 26.2,16.1 37.8,3.0 49.4,27.5 61.0,29.9 72.6,13.9 84.2,26.9 95.8,18.3 107.4,4.0 119.0,25.7 130.6,20.9 142.2,9.1 177.0,33.0"/></svg>

### `timoshenko-cantilever` — `tip_rel_err_pct`

- labels: `gate1-p1`
- solvers: PolyMesh
- values: 1.5
- sparkline: `▁`

## How to refresh

```sh
python3 bench/competitive/render_scoreboard.py
```

See [bench/competitive/README.md](../../bench/competitive/README.md).
