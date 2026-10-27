#!/bin/sh
# homemind-cpu-limit.service: caps every cpufreq policy at HM_CPU_MAX_KHZ (config/cpu-limit.env); empty = full speed.
# Real Pi5-01 2026-10-25 on a 5 V 3 A USB-C PD supply: EXAONE on all four cores at 2.4 GHz pulled the input under
# 4.63 V ("Undervoltage detected") and the board powered off twice; at 1.8 GHz none. With the official 5.1 V 5 A
# supply set HM_CPU_MAX_KHZ= (empty).
set -eu
max="${HM_CPU_MAX_KHZ:-}"
case "$max" in
  *[!0-9]*) echo "HM_CPU_MAX_KHZ must be a number in kHz or empty, not '$max'" >&2; exit 2 ;;
esac
for policy in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -f "$policy/scaling_max_freq" ] || continue
  if [ -n "$max" ]; then echo "$max" > "$policy/scaling_max_freq"; else cat "$policy/cpuinfo_max_freq" > "$policy/scaling_max_freq"; fi
  echo "${policy##*/}: max $(cat "$policy/scaling_max_freq") kHz (hardware $(cat "$policy/cpuinfo_max_freq") kHz)"
done
