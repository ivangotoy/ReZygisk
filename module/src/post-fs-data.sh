#!/system/bin/sh

set -e

MODDIR=${0%/*}
if [ "$ZYGISK_ENABLED" ]; then
  exit 0
fi

cd "$MODDIR"

create_sys_perm() {
  mkdir -p $1
  chmod 555 $1
  chcon u:object_r:system_file:s0 $1
}

export TMP_PATH=/data/adb/rezygisk
rm -rf "$TMP_PATH"

create_sys_perm $TMP_PATH

# INFO: We need to cleanup the module.prop to its default state before ReZygisk writes to it
#         so that it never duplicates the module status in there.
cp "$MODDIR/module.prop.bak" "$MODDIR/module.prop"

# INFO: Utilize the one with the biggest output, as some devices with Tango have the full list
#         in ro.product.cpu.abilist but others only have a subset there, and the full list in
#         ro.system.product.cpu.abilist
CPU_ABIS_PROP1=$(getprop ro.system.product.cpu.abilist)
CPU_ABIS_PROP2=$(getprop ro.product.cpu.abilist)

if [ "${#CPU_ABIS_PROP2}" -gt "${#CPU_ABIS_PROP1}" ]; then
  CPU_ABIS=$CPU_ABIS_PROP2
else
  CPU_ABIS=$CPU_ABIS_PROP1
fi

if [[ "$CPU_ABIS" == *"arm64-v8a"* || "$CPU_ABIS" == *"x86_64"* ]]; then
  ./bin/zygisk-ptrace64 monitor &
else
  # INFO: Device is 32-bit only

  ./bin/zygisk-ptrace32 monitor &
fi

# INFO: Other Zygisk modules' scripts run after the monitor is up, so that a slow one cannot
#         let zygote start before ReZygisk traces init
if [ "$(which magisk)" ]; then
  # INFO: A module script still running after the limit is left running in the
  #         background (never killed) and the next one starts. The watchdog's
  #         USR1 only ends the wait, so the trap stays set for good.
  limit=${REZYGISK_TEST_SCRIPT_LIMIT:-10}
  trap : USR1

  for file in ../*; do
    if [ -d "$file" ] && [ -d "$file/zygisk" ] && ! [ -f "$file/disable" ]; then
      if [ -f "$file/post-fs-data.sh" ]; then
        cd "$file"
        log -p i -t "zygisk-sh" "Manually trigger post-fs-data.sh for $file"

        # INFO: Don't propagate errexit
        set +e

        sh "$(realpath ./post-fs-data.sh)" &
        script_pid=$!
        ( sleep "$limit" && kill -USR1 $$ ) &
        watchdog_pid=$!

        wait "$script_pid"
        if kill -0 "$script_pid" 2>/dev/null; then
          log -p w -t "zygisk-sh" "post-fs-data.sh of $file still running after ${limit}s, continuing without it"
        fi

        kill "$watchdog_pid" 2>/dev/null

        # INFO: Re-enable errexit
        set -e

        cd "$MODDIR"
      fi
    fi
  done
fi

exit 0
