#!/bin/bash
# Collect everything this project changed, sorted by which repository it
# belongs to, into the Windows-side tree.
#
# The work is spread over four openvela repos plus a set of host-side
# scripts, and a fair amount of it is untracked -- so `git diff` alone does
# not capture it and no single backup does either. This writes both forms:
#
#   patch/  a diff per repository, to apply
#   files/  the actual files, to read or copy
#
#   ./export_changes.sh [destination]
#
# Default destination is the contest tree on C:, which is the one kept for
# submission.

set -u

DEST="${1:-/mnt/c/Users/杨文才/Desktop/OPENVELA/src/changes-2026-09-16}"
SRC="$HOME/openvela"

echo "exporting to: $DEST"
rm -rf "$DEST"
mkdir -p "$DEST"/{patch,files,host,docs}

# ── 01 program side: the agent, the driver, the board configuration ─────────
# Categorised by what was worked on rather than by path, because that is how
# it will be described in the submission.

echo
echo "=== 01 ai_agent ==="
cd "$SRC/packages/ai_agent" || exit 1
git diff > "$DEST/patch/01-ai_agent.patch" 2>/dev/null
mkdir -p "$DEST/files/ai_agent"
git status --short | awk '$1=="??"{print $2}' | while read -r f; do
  [ -e "$f" ] || continue
  mkdir -p "$DEST/files/ai_agent/$(dirname "$f")"
  cp -r "$f" "$DEST/files/ai_agent/$(dirname "$f")/" 2>/dev/null
done
echo "  tracked changes: $(git status --short | grep -c '^ M')"
echo "  untracked:       $(git status --short | grep -c '^??')"

echo
echo "=== 02 vendor_sifli (AUDCODEC driver + board config) ==="
cd "$SRC/vendor/sifli" || exit 1
git diff > "$DEST/patch/02-vendor_sifli.patch" 2>/dev/null
mkdir -p "$DEST/files/vendor_sifli"
for f in chips/sf32lb52/sf32lb_audcodec.c \
         chips/sf32lb52/include/sf32lb_audcodec.h \
         boards/sf32lb52/sf32lb52_lchspi_ulp/configs/agent/defconfig; do
  [ -e "$f" ] || continue
  mkdir -p "$DEST/files/vendor_sifli/$(dirname "$f")"
  cp "$f" "$DEST/files/vendor_sifli/$f"
done
echo "  tracked changes: $(git status --short | grep -c '^ M')"

echo
echo "=== 03 apps (pppd) ==="
cd "$SRC/apps" || exit 1
# Only the PPP app: the tree also has unrelated untracked third-party
# leftovers under testing/, which are not ours and must not travel with this.
git diff -- netutils/pppd include/netutils/pppd.h examples/pppd \
  > "$DEST/patch/03-apps-pppd.patch" 2>/dev/null
mkdir -p "$DEST/files/apps"
for f in $(git diff --name-only -- netutils/pppd include/netutils/pppd.h examples/pppd); do
  mkdir -p "$DEST/files/apps/$(dirname "$f")"
  cp "$f" "$DEST/files/apps/$f"
done
echo "  pppd files: $(git diff --name-only -- netutils/pppd include/netutils/pppd.h examples/pppd | wc -l)"

# ── 04 host side ───────────────────────────────────────────────────────────
# The PC half of the system: the PTY bridge, the voice pipeline, the LLM
# proxy, and the bring-up and recovery scripts. None of this lives in a
# repository, so this is its only copy.

echo
echo "=== 04 host scripts ==="
mkdir -p "$DEST/host"
for f in bridge.py bridge_restart.sh demo_up.sh \
         voice_bridge.py voice_ready.sh voice_restart.sh \
         mock_llm.py llm_proxy.py proxy_restart.sh \
         set_llm.sh setup_llm.sh test_llm.sh test_model.sh list_models.sh \
         device_ppp_mode3.py flash.py \
         pipeline_watchdog.sh soak.sh idle_control.sh hang_probe.sh \
         measure.sh rate_watch.sh nsh_run.py inventory.sh export_changes.sh \
         asr_selftest.py asr_rate_test.py mock_test.py load_test.py \
         show_crash.sh show_hang.sh show_tls_tail.sh show_skills_prompt.sh \
         watchdog_test.sh nat_counter_test.sh capture_device_traffic.sh \
         dns_test.py; do
  [ -e "$HOME/$f" ] && cp "$HOME/$f" "$DEST/host/"
done
echo "  copied: $(ls -1 "$DEST/host" | wc -l) files"

# ── 05 notes ───────────────────────────────────────────────────────────────
echo
echo "=== 05 notes ==="
cp "$HOME/patches/"*.patch "$DEST/patch/" 2>/dev/null
cp "$HOME/patches/"*defconfig* "$DEST/files/" 2>/dev/null
echo "  earlier patch set copied alongside (dated 2026-09-13)"

du -sh "$DEST" 2>/dev/null
echo
echo "done"
