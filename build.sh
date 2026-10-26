#!/usr/bin/env bash
# Quantum OS: one-command, resumable build.
#   ./build.sh            run every stage that is not finished yet
#   ./build.sh gcc1 glibc run only the named stages (re-runs them)
#   ./build.sh --reset    forget progress (does not delete built files)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/scripts/00-env.sh"
source "$HERE/scripts/01-fetch.sh"
source "$HERE/scripts/02-toolchain.sh"
source "$HERE/scripts/03-image.sh"

[ "$(id -u)" -ne 0 ] || { echo "Do not run as root. Use a normal user."; exit 1; }

# Order matters.
STAGES=(prepare fetch binutills gcc1 headers glibc libstdcxx toolchain_check
        busybox kernel live initramfs iso iso_test)

preflight() {
  [ -n "${QOS_SKIP_PREFLIGHT:-}" ] && return 0
  local missing=() c
  for c in gcc g++ make bison flex gawk m4 patch makeinfo perl python3 bc cpio \
           wget gpg xz bzip2 tar sed rsync mksquashfs xorriso grub-mkrescue mformat; do
    command -v "$c" >/dev/null || missing+=("$c")
  done
  [ -f /usr/include/openssl/evp.h ] || missing+=("libssl-dev")
  [ -f /usr/include/libelf.h ] || missing+=("libelf-dev")
  if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing host tools: ${missing[*]}"
    echo "On Debian/Ubuntu run:"
    echo "  sudo apt install -y build-essential bison flex gawk texinfo m4 patch perl python3 \\"
    echo "    wget gnupg xz-utils bzip2 cpio rsync squashfs-tools xorriso mtools dosfstools \\"
    echo "    grub-common grub-pc-bin grub-efi-amd64-bin bc libssl-dev libelf-dev dwarves \\"
    echo "    qemu-system-x86 ovmf"
    exit 1
  fi
  [ "$(df -BG --output=avail "$QOS" | tail -1 | tr -dc 0-9)" -ge 40 ] \
    || echo "WARNING: less than 40 GB free; later stages need much more."
}

run_stage() {
  local s="$1" log="$QOS/logs/$1.log"
  echo "==> [$(date +%H:%M:%S)] $s  (log: $log)"
  local start=$SECONDS
  set +e
  ( set -euo pipefail; "$s" ) >"$log" 2>&1
  local rc=$?
  if [ $rc -ne 0 ]; then
    echo "!! Stage '$s' FAILED (exit $rc). Last 40 log lines:"
    tail -n 40 "$log"
    echo "Fix the problem and run ./build.sh again; finished stages are skipped."
    exit $rc
  fi
  touch "$QOS/state/$s.done"
  echo "    done in $(( (SECONDS-start)/60 )) min"
}

if [ "${1:-}" = "--reset" ]; then rm -f "$QOS"/state/*.done; echo "Progress reset."; exit 0; fi

preflight
if [ $# -gt 0 ]; then
  for s in "$@"; do run_stage "$s"; done
else
  for s in "${STAGES[@]}"; do
    [ -f "$QOS/state/$s.done" ] && { echo "==> $s already done, skipping"; continue; }
    run_stage "$s"
  done
fi
echo "Quantum OS build finished all current stages."
[ -f "$OUT/$QOS_IMG" ] && echo "ISO ready: $OUT/$QOS_IMG ($(du -h "$OUT/$QOS_IMG" | cut -f1))"
exit 0
