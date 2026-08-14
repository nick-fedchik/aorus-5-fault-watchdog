#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  printf 'capture helper must run as root\n' >&2
  exit 1
fi

base_dir="${1:?incident directory is required}"
timestamp="$(date --iso-8601=seconds)"
safe_timestamp="${timestamp//:/-}"
out_dir="${base_dir}/${safe_timestamp}"

mkdir -p "${out_dir}"
printf '%s\n' "${timestamp}" > "${out_dir}/timestamp.txt"
cat /proc/cmdline > "${out_dir}/proc-cmdline.txt"
cat /sys/power/mem_sleep > "${out_dir}/mem_sleep.txt"

for item in control runtime_status runtime_suspended_time wakeup; do
  source_path="/sys/bus/pci/devices/0000:00:14.0/power/${item}"
  [[ -r "${source_path}" ]] || continue
  cat "${source_path}" > "${out_dir}/00:14.0-power-${item}.txt"
done

for pci_device in 0000:01:00.0 0000:01:00.1; do
  for item in control runtime_status runtime_suspended_time; do
    source_path="/sys/bus/pci/devices/${pci_device}/power/${item}"
    [[ -r "${source_path}" ]] || continue
    cat "${source_path}" > "${out_dir}/${pci_device#0000:}-power-${item}.txt"
  done
done

for pci_device in 0000:00:14.0 0000:01:00.0 0000:01:00.1; do
  source_path="/sys/bus/pci/devices/${pci_device}/d3cold_allowed"
  [[ -r "${source_path}" ]] || continue
  cat "${source_path}" > "${out_dir}/${pci_device#0000:}-d3cold_allowed.txt"
done

if [[ -r /run/aorus-fault-watchdog/startup-nvidia-smi.txt ]]; then
  cp /run/aorus-fault-watchdog/startup-nvidia-smi.txt \
    "${out_dir}/startup-nvidia-smi.txt"
fi
if command -v nvidia-smi >/dev/null 2>&1; then
  timeout --signal=TERM --kill-after=2s 10s nvidia-smi -q \
    > "${out_dir}/nvidia-smi-q.txt" 2>&1 || true
fi

journalctl -b -k --no-pager > "${out_dir}/journalctl-k-b.txt"
journalctl -b -k --no-pager | grep -nE \
  'PM: suspend|resume|xhci_hcd 0000:00:14.0|DMAR|TRB|HC died|r8152|Bluetooth|NVRM|Xid|GSP|nv_acpi|runtime.resume' \
  > "${out_dir}/journalctl-k-filtered.txt" || true

lsusb -t > "${out_dir}/lsusb-t.txt" 2>&1 || true
lsusb > "${out_dir}/lsusb.txt" 2>&1 || true
lspci -vvv -s 00:14.0 > "${out_dir}/lspci-vvv-00:14.0.txt" 2>&1 || true
lspci -vvv -s 01:00.0 > "${out_dir}/lspci-vvv-01:00.0.txt" 2>&1 || true
cat /proc/interrupts > "${out_dir}/proc-interrupts.txt"
cat /proc/bus/input/devices > "${out_dir}/proc-bus-input-devices.txt"

for controller in 0000:00:14.0 0000:00:0d.0; do
  debug_dir="/sys/kernel/debug/usb/xhci/${controller}"
  [[ -d "${debug_dir}" ]] || continue
  find "${debug_dir}" -maxdepth 4 -type f -printf '%p\n' \
    > "${out_dir}/debugfs-${controller#0000:}-file-list.txt" || true
done

printf '%s\n' "${out_dir}"
