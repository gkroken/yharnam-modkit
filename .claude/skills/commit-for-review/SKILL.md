---
name: commit-for-review
description: Commit one finished unit of work in the tool repo or the port, then wait for the reviewer's verdict before starting anything else. Use whenever a change is ready to commit.
---

# Commit, then wait for the review

1. **Run the test gate:** `bash tools/test-port.sh --quiet`. Run it in the background; it takes about 5 minutes.
   - It must end with "Matches the baseline".
   - Never change `tools/test-baseline.txt` to make it pass.
2. **Commit one logical change.** The code, its test and its docs go in the same commit.
   - Commit as `git -c user.name=gard1 -c user.email=89836666+gkroken@users.noreply.github.com commit -F <msgfile>`. Write the message file in the scratchpad.
   - Message: `<scope>: <what and why>`. Scopes: plugins, api, lua, edits, sdk, docs, build, test, plan.
   - Every claim in the message must be true, and must say how it was tested ("in game: ...", "not yet tried in game").
   - A fix cites its finding IDs, for example `(tool-d34a1a6-N1)`.
   - End the message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
   - **Port commits:**
     1. Commit in `port/`.
     2. Commit the new submodule pointer in the tool repo, together with whatever in the tool uses it.
   - Never amend, rebase or force-push. Never push without the user's OK.
3. **Leave both working trees clean.** The reviewer builds this same checkout.
   - `git status --short` and `git -C port status --short` may show only the `fsr-vulkan` line.
   - Stash any unfinished work.
4. **Wait for the review.** Run `bash tools/wait-review.sh tool` (or `port`) in the background.
   - It prints only the verdict and the finding headings when the report arrives.
   - While waiting, only read and plan. Change no file.
5. **On the verdict:**
   - **PASS with no findings:** go on.
   - **Findings:** read just that report's findings section. Fix forward in a new commit that cites the IDs.
   - **CHANGES_REQUIRED:** fix it before any new work.
