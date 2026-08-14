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

## 3. Enrolling the laptop's key

`sshd` on the box allows `publickey` and `password`; `PermitRootLogin` is
`prohibit-password` and keyboard-interactive is off. Only `livingroom-pc`'s key
is in `authorized_keys` today, so the laptop must enrol its own once:

```sh
ssh-keygen -t ed25519 -C laptop-to-hunter-pc      # if the laptop has no key yet
ssh-copy-id -i ~/.ssh/id_ed25519.pub hunter@hunter-pc.local
```

`ssh-copy-id` prompts for the `hunter` account password. **That password is
delivered out of band and is never committed here.** After enrolment, verify
key-only login works:

```sh
ssh -o BatchMode=yes hunter@hunter-pc.local 'hostname; nvidia-smi -L'
```

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

The handoff says to STOP on a mismatch, so this is flagged rather than waved
through: the topology half of the gate passes and the volume error is *lower*
than the recorded figure, not worse. The recorded figure was measured on the
laptop's MSVC build; this is a gcc Release build of the same commit, which is
the most likely source of the difference, but that has not been proven here.
Resolve it before trusting labels produced on this box for graded_tet rows.

## 5. Notes for long unattended runs

- `sleep.target` / `suspend.target` are `static` and no `IdleAction` is
  configured, so the box does not suspend on idle; the GNOME session blanks the
  screen after 300 s, which does not affect an SSH-launched run.
- Prefer `systemd-run --user` or `tmux` for anything longer than the SSH
  session, so a dropped link does not kill a 1–3 day run.
- The box's `authorized_keys` already trusts `livingroom-pc`, so that machine can
  be used as a jump host if the laptop's link to this subnet is flaky.
