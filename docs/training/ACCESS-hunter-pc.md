# LAN access — `hunter-pc` (the 3080 Ti box)

For the agent driving training from the laptop. Written 2026-08-14, verified from
a second machine on the same LAN (`livingroom-pc`) on that date.

**This file is in a PUBLIC repository. It carries no secret: no private key, no
password, no external hostname.** The box is reachable only from the LAN — do not
add credentials, port-forwards, or a public address to this file.

## 1. Destination

| Field | Value |
|---|---|
| Hostname | `hunter-pc` |
| mDNS name | `hunter-pc.local` (preferred — survives a DHCP lease change) |
| Address | `192.168.0.123/24` (DHCP lease, LAN only) |
| MAC (`eno1`) | `10:7c:61:72:db:b3` (use this to re-find the box if the lease moves) |
| User | `hunter` |
| Port | `22` |
| OS | Fedora, `6.19.10-300.fc44.x86_64`, gcc/ccache toolchain |
| GPU | RTX 3080 Ti (12 GB), CUDA sm_86 |
| CPU / RAM | Ryzen 5 7600X3D, 6C/12T |
| Repo | `/home/hunter/Desktop/Polyhedral-FEA` |

Suggested `~/.ssh/config` entry on the laptop:

```
Host hunter-pc
    HostName hunter-pc.local
    User hunter
    Port 22
    ServerAliveInterval 30
    ServerAliveCountMax 6
```

## 2. Host key fingerprints — verify these on first connect

Do not blind-accept the host key; compare against:

```
ED25519  SHA256:jLxLPeM5rJ8bP9E/aEYWIfKORvGsgVSccgRqis/jbeg
RSA      SHA256:26/70fjfVvMR1FRfPL01DeacX672gv1ezxQljEkqRnk
ECDSA    SHA256:SFfYeLRbkf0QRkBmveVKC3F3pT89ZfnDC7vaApPJwz0
```

## 3. Enrolling a key — this is what "it's not working" was

The laptop (`192.168.0.122`) **does** reach this box. Its attempt at
2026-08-14 11:25 got through TCP and key exchange (`op=start … res=success` in
the audit log) and failed at exactly one step:

```
op=pubkey acct="hunter" addr=192.168.0.122 terminal=ssh res=failed
Connection closed by authenticating user hunter 192.168.0.122 port 61583 [preauth]
```

So the network, the firewall, the address and the account are all fine: the
laptop's public key simply is not in this account's `authorized_keys`, and a
non-interactive agent never falls back to the password prompt. Nothing about the
box needs changing — the key needs enrolling.

`sshd` allows `publickey` and `password`; `PermitRootLogin` is
`prohibit-password` and keyboard-interactive is off. `LogLevel VERBOSE` is now
set (`/etc/ssh/sshd_config.d/10-loglevel-verbose.conf`), so a future failure logs
the fingerprint of the key that was offered instead of only "Permission denied".

### The repo is the enrolment channel

A public key is not a secret, so it can travel in this public repository — which
is the only channel an agent on another machine can use unattended. From the
laptop:

```sh
ssh-keygen -t ed25519 -C laptop-to-hunter-pc      # only if it has no key yet
cp ~/.ssh/id_ed25519.pub docs/training/authorized-keys/laptop.pub
git add docs/training/authorized-keys/laptop.pub && git commit && git push
```

Then, on this box (a human runs this; enrolment is never automatic):

```sh
git pull && ./scripts/enroll_lan_keys.sh --dry-run   # names each key + fingerprint
./scripts/enroll_lan_keys.sh                         # appends the new ones
```

Re-running is a no-op, non-keys are rejected, and a key already present is
reported rather than duplicated. Verified end to end on 2026-08-14 with
`livingroom-pc`'s key, which failed `op=pubkey` the same way the laptop did and
logged in immediately after enrolment:

```sh
$ ssh -o BatchMode=yes hunter@hunter-pc.local 'hostname; nvidia-smi -L'
LOGIN-OK as hunter@hunter-pc
NVIDIA GeForce RTX 3080 Ti
```

`ssh-copy-id -i ~/.ssh/id_ed25519.pub hunter@hunter-pc.local` also works if
someone can type the account password interactively; that password is delivered
out of band and is never committed here.

## 4. Verified state of the box (2026-08-14)

- `sshd` enabled + active, listening on `0.0.0.0:22` and `[::]:22`.
- `firewalld` zone `FedoraWorkstation`: `ssh` and `mdns` both permitted.
  mDNS was blocked before this date, which is why `hunter-pc.local` used to fail
  from other machines; `avahi-resolve -4 -n hunter-pc.local` now answers
  `192.168.0.123` from `livingroom-pc`. (That box's firewall needed `mdns`
  opened too, and was.)
- Repo at `cc38ecd`, clean of unpushed commits; `origin/master` is the same SHA,
  so the laptop's cycle of fixes through `798ef79` is fully pushed.
- `polymesh` + `polymesh_testlab` rebuilt at that SHA (Release, OCC on).
- Procedural corpus regenerated: 32 STEP parts, 96 `*.case.json`. It is a
  generated artifact and absent from a fresh clone — any new box must run
  `python3 scripts/gen_primitive_corpus.py` before labelling, or `run_batch.py`
  aborts on a missing `box_hole_s0_c0.case.json`.
- `pyvista` is installed here; `livingroom-pc` lacks it (warehouse previews
  would silently render nothing there).

### Smoke gate from `HANDOFF-3080ti.md` §0.4 — one value disagrees

```
polymesh solve bench/geometries/corpus/primitives/sphere_box_s0.step \
    -h 0.0036 --mesher graded
  → open=0 nonmanifold=0            as required
  → rel_err = 5.214e-05             handoff requires ~1.1e-04
polymesh mesh tests/fixtures/parts/icecream_cone.step -h 0.010
  → completes, no "buried" refusal  as required
```

The handoff says to STOP on a mismatch, so it was measured rather than waved
through, and the answer is in §4.1: the gate value differs because the toolchain
differs, the difference is real, and it is not confined to this one number.

### 4.1 Resolved 2026-08-14 — gcc and MSVC do not agree on every mesh

Three campaigns were run on this box against 24 `(part, cfg)` pairs already
labelled on the laptop, at the batch-4 grid (`h_rel` 0.08, `hybrid_zoo` and
`graded_tet`, 12 parts, one case each): `bench/campaigns/xcheck-gcc-1`, then
`xcheck-omp1` and `xcheck-omp12` re-running the divergent parts at
`OMP_NUM_THREADS` 1 and 12. None are named `advisor-*`, so `run_batch.py`'s
permanent pair suppression never sees them.

**This box is deterministic.** Every value in the three runs is identical —
element counts, volume errors, statuses — so reduction order, thread count and
run-to-run noise are ruled out. Two consecutive `polymesh solve` invocations of
the gate command also return the same `rel_err = 5.214e-05` exactly.

**19 of 24 pairs reproduce the MSVC labels; 5 do not.**

| part | mesher | MSVC | gcc |
| --- | --- | --- | --- |
| `sphere_box_s0_c0` | graded_tet | `solve_fail`, no mesh | `ok`, 21,256 elems, err 4.7e-04 |
| `stepped_shaft_s2_c0` | hybrid_zoo | 264 elems, err 0.05742 | 200 elems, err 0.06195 |
| `stepped_shaft_s0_c0` | graded_tet | 12,671 elems | 12,662 elems |
| `stepped_shaft_s2_c0` | graded_tet | 11,171 elems | 11,173 elems |
| `plate_notch_s0_c0` | graded_tet | 6,426 elems | 6,424 elems |

The element-count deltas are single digits, but they propagate: the worst
`geometry_volume_err` change over the 24 pairs is +94 % (`stepped_shaft_s0_c0`
graded_tet, 5.06e-05 → 9.81e-05), and one status flips outright. So the two
builds make different decisions at floating-point-sensitive tie-breaks in the
mesher, at a rate of about 1 pair in 5 on this grid.

What follows for labelling, and it is a constraint rather than a defect:

- Labels from this box are **internally consistent and reproducible**, and are
  fine for any campaign generation that is labelled entirely here.
- They are **not interchangeable per-pair with the existing corpus**, all of
  which was labelled on the laptop's MSVC build
  (`scripts/advisor/run_batch.py:616`). Never re-label part of a family here and
  leave the rest MSVC: the advisor splits by family, so a mixed family puts a
  toolchain artefact inside a fold.
- The gate value in `HANDOFF-3080ti.md` §0.4 is an MSVC figure. On gcc the gate
  is `rel_err = 5.214e-05`, `open=0 nonmanifold=0`, and that is what a future
  bring-up on this box should compare against.

## 5. Notes for long unattended runs

- `sleep.target` / `suspend.target` are `static` and no `IdleAction` is
  configured, so the box does not suspend on idle; the GNOME session blanks the
  screen after 300 s, which does not affect an SSH-launched run.
- Prefer `systemd-run --user` or `tmux` for anything longer than the SSH
  session, so a dropped link does not kill a 1–3 day run.
- The box's `authorized_keys` already trusts `livingroom-pc`, so that machine can
  be used as a jump host if the laptop's link to this subnet is flaky.

## 6. Launching a batch on this box

`run_batch.py --dry-run` was exercised here after the corpus regeneration and
plans correctly: the cfg_id mirror re-verifies against 3,885 recorded rows and
the truth gate is reached. Two things it reports that the driver must decide:

- The truth campaign is **216/288 pairs, 3 configs incomplete** — the
  regenerated corpus is wider than the one `advisor-truth-0` was solved against,
  so `run_batch.py` will re-run the truth gate before any batch. Budget for it.
- `--campaign-template` has no usable default in this clone: the built-in
  `bench/campaigns/advisor-pilot-1/campaign.json` does not exist. Pass a real
  one, e.g. `bench/campaigns/advisor-batch-3-template/campaign.json`.

Shard sizing in `run_batch.py` (`SHARDS = 4`, `OMP_THREADS_PER_SHARD = 2`) is
hard-coded for a 6C/12T machine, which is exactly this box; override with
`--shards/--omp-threads` when driving anything else.
