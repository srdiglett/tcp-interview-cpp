#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
state="$root/.yocto-qemu"
base=https://downloads.yoctoproject.org/releases/yocto/yocto-5.0.17/machines/qemu/qemuarm64
disk=core-image-full-cmdline-qemuarm64.rootfs-20260402133935.ext4
kernel=Image-qemuarm64.bin
mode="${1:-start}"
mkdir -p "$state"
case "$mode" in
  prepare)
    command -v qemu-img >/dev/null
    for file in "$kernel" "$disk"; do
      if [[ ! -f "$state/$file" ]]; then
        curl -fL --retry 3 "$base/$file" -o "$state/$file.part"
        curl -fL --retry 3 "$base/$file.sha256sum" -o "$state/$file.sha256sum"
        expected="$(awk '{print $1}' "$state/$file.sha256sum")"
        actual="$(shasum -a 256 "$state/$file.part" | awk '{print $1}')"
        [[ "$actual" == "$expected" ]] || { echo 'SHA256 mismatch' >&2; exit 1; }
        mv "$state/$file.part" "$state/$file"
      fi
    done
    # El original queda intacto: QEMU guarda cambios en un overlay persistente.
    if [[ ! -f "$state/device.qcow2" ]]; then
      qemu-img create -f qcow2 -F raw -b "$state/$disk" "$state/device.qcow2"
    fi
    if [[ ! -f "$state/id_ed25519" ]]; then
      ssh-keygen -q -t ed25519 -N '' -C yocto-local-demo -f "$state/id_ed25519"
    fi
    echo 'Preparado. Arranca con: bash scripts/yocto_qemu.sh start'
    ;;
  start)
    [[ -f "$state/device.qcow2" ]] || { echo 'Ejecuta primero prepare' >&2; exit 1; }
    # TCG funciona sin privilegios de virtualizacion. Maquina ARM64 generica.
    exec qemu-system-aarch64 -machine virt -cpu cortex-a57 -accel tcg \
      -smp 2 -m 512 -nographic \
      -kernel "$state/$kernel" \
      -append 'root=/dev/vda rw console=ttyAMA0 ip=dhcp' \
      -drive "file=$state/device.qcow2,if=none,id=disk0,format=qcow2" \
      -device virtio-blk-pci,drive=disk0 \
      -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
      -device virtio-net-pci,netdev=net0
    ;;
  key) cat "$state/id_ed25519.pub" ;;
  *) echo 'Uso: bash scripts/yocto_qemu.sh prepare|start|key' >&2; exit 2 ;;
esac
