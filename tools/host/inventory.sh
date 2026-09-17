#!/bin/bash
# What the submission actually consists of, repo by repo.
#
# The work is spread over four openvela repos plus a pile of host-side
# scripts, and some of it is untracked -- which means no version control is
# protecting it and `git diff` will never show it. Worth knowing before
# anything is packaged.

cd "$HOME/openvela" || exit 1

for r in packages/ai_agent vendor/sifli apps nuttx; do
  echo "=== $r ==="
  mod=$(git -C "$r" status --short 2>/dev/null | grep -c '^ M')
  unt=$(git -C "$r" status --short 2>/dev/null | grep -c '^??')
  echo "  modified: $mod   untracked: $unt"
  git -C "$r" status --short 2>/dev/null | grep '^ M' | sed 's/^/    M /'
  git -C "$r" status --short 2>/dev/null | grep '^??' | sed 's/^/    ? /'
  echo
done

echo "=== host-side scripts (NOT in any repo) ==="
ls -1 "$HOME"/*.py "$HOME"/*.sh 2>/dev/null | xargs -n1 basename | sort | sed 's/^/    /'

echo
echo "=== contest repo ==="
CR="/mnt/c/Users/杨文才/Desktop/OPENVELA/src/contest2026_286_hajiminanbeilvdou"
git -C "$CR" log --oneline -1 2>/dev/null
echo "  tracked files under app/:"
git -C "$CR" ls-files app | sed 's/^/    /'
echo "  logs (untracked = not submitted):"
git -C "$CR" status --short logs | sed 's/^/    /'
