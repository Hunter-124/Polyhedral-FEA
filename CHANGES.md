# CHANGES.md — Agent instructions for external PRs

**Audience:** AI agents acting for external contributors. Not the repo owner
(owner commits/pushes `master` directly). Not a human git tutorial.

**Default branch:** `master`  
**Canonical remote:** `https://github.com/Hunter-124/Polyhedral-FEA.git`

If you are the owner's agent on the owner's machine: **ignore this file** and
work on `master` as directed.

---

## Mission

Deliver a **PR into `master`** whose commits are **only this contributor’s
work**, based on a **correct, up-to-date clone**. Most failures are:

1. Wrong clone (old zip, nested folder, wrong remote, detached HEAD, copy of
   someone else’s dirty tree).
2. PR that includes unrelated history, force-rewrites, or “merged master by
   rebasing the whole universe.”
3. Branch not started from current `master`.

---

## Hard rules

| # | Rule |
|---|---|
| 1 | **Never** commit or push to `master` as an external contributor. |
| 2 | **Never** `git push --force` to `master`. Force-push only your **own** branch if you must, and prefer not to. |
| 3 | **Never** rewrite history with `git rebase` of the entire repo / interactive rebase of unrelated commits. Prefer merge or a short rebase **of your branch only** onto latest `master`. |
| 4 | **Never** hardcode benchmark answers into `src/` or `apps/`. |
| 5 | **Never** add AI attribution (`Co-Authored-By`, “Generated with …”) to commits or PRs. |
| 6 | **Never** commit `build/`, secrets, or binary dumps. |
| 7 | Commit as the **contributor’s** `user.name` / `user.email`, not as Hunter-124. |
| 8 | One logical change per PR. |

Also read **CONTRIBUTING.md** for layout and engineering standards.

---

## Correct clone (do this first — most failures start here)

Verify you are in a real git checkout of **this** repo:

```bash
git rev-parse --is-inside-work-tree    # must print: true
git remote -v                         # must show Hunter-124/Polyhedral-FEA (or their fork)
git branch -vv                        # should know about master
git status
```

**If any of that fails**, stop editing code. Fix the workspace:

```bash
# Preferred: fresh clone of the canonical repo (write access) or their fork
git clone https://github.com/Hunter-124/Polyhedral-FEA.git
cd Polyhedral-FEA
git checkout master
git pull origin master
```

Fork path (no write access to upstream):

```bash
git clone https://github.com/<THEIR_USER>/Polyhedral-FEA.git
cd Polyhedral-FEA
git remote add upstream https://github.com/Hunter-124/Polyhedral-FEA.git
git fetch upstream
git checkout master
git merge --ff-only upstream/master || git reset --hard upstream/master
```

**Reject these as the work root:** random Downloads zips, nested
`Polyhedral-FEA/Polyhedral-FEA`, a directory with no `.git`, a clone of the
wrong project, or a tree where `git log -1` is months behind without a pull.

---

## Start work (every session)

```bash
git fetch origin          # or: git fetch upstream && git fetch origin
git checkout master
git pull --ff-only origin master   # fork: merge/ff from upstream/master first
git checkout -b <user>/<short-topic>
```

Branch name example: `alex/tet-quality`. Work **only** on that branch.

---

## While coding

- Stage **named files** you changed. Avoid blind `git add .` if `build/` or junk is present.
- Commit messages: imperative, what/why (`Fix tet volume orientation on fill`).
- Before claiming done (from repo root):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build && ctest --test-dir build
```

C++: `clang-format -i` on touched `*.cpp` / `*.hpp` / `*.cu` under `apps/`, `src/`, `tests/`.

---

## Update with latest `master` (without wrecking history)

When `master` moved and the PR is behind, **update the branch**, do not rebuild
the repo from scratch and do not rebase unrelated commits.

**Preferred (simple, safe for novices’ agents):**

```bash
git fetch origin                 # fork: also fetch upstream
git checkout <user>/<short-topic>
git merge origin/master          # fork: git merge upstream/master
# fix conflicts in YOUR files only
cmake --build build && ctest --test-dir build
git push origin <user>/<short-topic>
```

**Allowed alternative:** rebase **only this feature branch** onto latest master:

```bash
git fetch origin
git checkout <user>/<short-topic>
git rebase origin/master
# fix conflicts, then:
git push --force-with-lease origin <user>/<short-topic>
```

Use `--force-with-lease` only on **their** branch after that rebase. Never on
`master`.

**Do not:**

- `git pull` that creates a mess of unrelated merge commits from bad remotes
- copy files into a second half-broken clone and open a PR from that
- squash/rebase the entire project history
- reset `master` to match a local experiment

---

## Open the PR

```bash
git push -u origin <user>/<short-topic>
gh pr create --base master --title "<short title>" --body "<what / why / how tested>"
```

PR must be:

- **base:** `Hunter-124/Polyhedral-FEA` → `master`
- **head:** their branch (same repo or fork)
- **diff:** only their feature commits + intentional merge/rebase with master

If the PR “Files changed” tab shows huge unrelated churn or half the tree, the
clone/branch is wrong — stop and re-clone, cherry-pick **only** their commits
onto a clean branch from current `master`, open a new PR.

---

## Merge responsibility

External contributors (and their agents) **own**:

1. Clean clone and correct branch point.
2. Green build/tests on the branch.
3. Conflict resolution when updating from `master` (merge or short rebase above).
4. CI green on the PR.

They may merge the PR **if** they have permission and CI is green and the PR
is only their change. Prefer GitHub’s **“Create a merge commit”** or
**“Squash and merge”** on the PR UI — do **not** rewrite `master` history
locally and force-push.

---

## Agent system prompt (paste this)

```text
You are an agent for an external PolyMesh contributor.
Read CHANGES.md (this policy) and CONTRIBUTING.md (code map). Ignore owner-on-master workflows.

1) Verify a correct git clone of Hunter-124/Polyhedral-FEA (or their fork). If the workspace is not a clean, up-to-date git checkout, re-clone before any edits.
2) Branch from current master: <user>/<short-topic>. Never commit to master.
3) Implement the task. Stage only intended files. No AI attribution in commits.
4) cmake + ctest green from repo root. clang-format touched C++.
5) Push branch. Open PR into master. If behind master: merge origin/master (or short rebase of THIS branch only) — do not rewrite whole-repo history.
6) PR diff must contain only this work. If the diff is polluted, start over from a fresh branch off master and re-apply only the real changes.
```
