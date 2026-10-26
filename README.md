# Quantum OS build project

Build system for Quantum OS: a Linux-based OS compiled from upstream source
(kernel.org Linux, GNU toolchain, BusyBox) with our own configs, branding and scripts.

## Build the ISO (Linux machine or VM, normal user, not root)

    ./build.sh

Needs: 8 GB RAM, 4+ cores, ~60 GB free disk, internet. It checks host tools first and
prints the exact `apt install` line if anything is missing. Expect a few hours.

Result: `out/quantum-os-1.0-x86_64.iso` (hybrid: legacy BIOS + UEFI, bootable from CD or USB).

- Resumable: if a stage fails, fix the cause and run `./build.sh` again.
- Re-run one stage: `./build.sh kernel`.  Forget progress: `./build.sh --reset`.
- Logs: `logs/<stage>.log`.  Progress markers: `state/`.
- Try it: `./scripts/test-qemu.sh` (BIOS) or `./scripts/test-qemu.sh uefi`.
  VirtualBox/VMware: create a Linux (64-bit) VM, 1+ GB RAM, attach the ISO.

## Stages

1. prepare, fetch, binutills, gcc1, headers, glibc, libstdcxx, toolchain_check
   (our own cross-toolchain: binutils, GCC, Linux headers, glibc)
2. busybox (shell, init, core utilities, built with our toolchain)
3. kernel (Linux 6.18 LTS, drivers built in for VirtualBox/VMware/QEMU/real PCs)
4. live (branded root filesystem), initramfs (finds the medium, mounts it)
5. iso (squashfs + GRUB BIOS/UEFI image), iso_test (boots it in QEMU and checks)

## What this ISO is (milestone 1)

A branded, text-mode Quantum OS live system. Boots to a shell with hostname `quantum`,
`/etc/os-release` = Quantum OS, Quantum motd/boot menu, DHCP networking.

Note: the final ISO image is assembled with the host's `grub-mkrescue`. Compiling GRUB
from source is a later hardening step.

## Coming next

systemd base system, graphics stack + KDE Plasma (Windows 11 style), Quantum theme,
icons, wallpapers, logo, boot splash, apps, app store, Wine, graphical installer,
Welcome app.

## Layout

    build.sh            one-command resumable build
    scripts/            00-env (versions), 01-fetch, 02-toolchain, 03-image, test-qemu
    config/system/      os-release, motd, inittab, rcS, profile (branding + startup)
    config/initramfs/   init (live boot logic)
    config/grub/        grub.cfg (boot menu)

## Legal

No Microsoft logos, code, fonts or assets. Components keep their upstream licenses
(Linux: GPLv2, GCC/binutils/GRUB: GPLv3, glibc: LGPL, BusyBox: GPLv2).
