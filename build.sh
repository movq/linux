#!/bin/bash

set -ex

export PATH="$PATH:$HOME/.local/aarch64-toolchain/bin"

make O=out ARCH=arm64 CROSS_COMPILE=aarch64-unknown-linux-gnu- -j16
doas cp out/arch/arm64/boot/Image.gz ~/.local/var/pmbootstrap/chroot_rootfs_google-taimen/boot/vmlinuz 
doas env PATH="$PATH:$HOME/.local/aarch64-toolchain/bin" \
    make O=out ARCH=arm64 CROSS_COMPILE=aarch64-unknown-linux-gnu- \
      INSTALL_PATH=$HOME/.local/var/pmbootstrap/chroot_rootfs_google-taimen/boot \
      INSTALL_MOD_PATH=$HOME/.local/var/pmbootstrap/chroot_rootfs_google-taimen \
      INSTALL_DTBS_PATH=$HOME/.local/var/pmbootstrap/chroot_rootfs_google-taimen/boot/dtbs \
      modules_install dtbs_install
