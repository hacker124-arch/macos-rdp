#!/usr/bin/env bash
# Quantum OS build. Self-contained: the scripts/ and config/ folders are embedded at the end of this file.
_HERE="$(cd "$(dirname "$0")" && pwd)"
if [ ! -f "$_HERE/scripts/00-env.sh" ]; then
  echo "Unpacking embedded project files..."
  sed '1,/^__QOS_PAYLOAD__$/d' "$0" | base64 -d | tar xz -C "$_HERE" || { echo "Unpack failed"; exit 1; }
fi
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
__QOS_PAYLOAD__
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
AA==
