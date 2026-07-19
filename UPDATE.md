# Updating Hyprland (patched fork)

## How to know there's a new stable release

```bash
cd ~/del/hyprland
git fetch --tags upstream
git tag -l 'v[0-9]*.[0-9]*.[0-9]*' --sort=-v:refname | head -5
```

Compare with `git tag -l 'working-v*' --sort=-v:refname | head -3` to see what you've already built.

You can also watch https://github.com/hyprwm/Hyprland/releases or subscribe to the repo's RSS feed.

## Step-by-step update

```bash
# 1. Fetch upstream tags
cd ~/del/hyprland
git fetch --tags upstream

# 2. Check if the patch still applies cleanly (dry run)
git checkout invisible-ss
git merge-base --is-ancestor v0.55.4 HEAD  # replace with YOUR current tag
# If this says "fatal: Not a valid commit name", you already rebased past it

# 3. Rebase onto the new tag (e.g. v0.57.0)
git rebase v0.57.0
# Three outcomes:
#   - Clean: git log --oneline shows your 3 commits on top of new tag
#   - Conflict: resolve each file, git add, git rebase --continue
#   - Disaster: git rebase --abort, you're back on old tag

# 4. Build and install
make release PREFIX=/usr
sudo make install PREFIX=/usr

# 5. Tag your success
git tag working-v0.57.0   # match the version you just built

# 6. Push to GitHub
git push origin invisible-ss --tags

# 7. Log out and back in for the new binary to take effect
```

## If something goes wrong

```bash
# Rollback to last known working version
git checkout working-v0.55.4   # whatever your last good tag was
make release PREFIX=/usr
sudo make install PREFIX=/usr

# Or abort a bad rebase entirely
git rebase --abort
```
