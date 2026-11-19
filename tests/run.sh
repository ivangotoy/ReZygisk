#!/bin/sh
# Dependency-free checks for the module scripts and helpers.
#   sh tests/run.sh
# Runs the real module scripts against PATH stubs inside a scratch dir under
# $TMPDIR. Checks whose tool is missing print SKIP. With REZYGISK_REQUIRE_CC=1
# a skipped C case fails the run.

R=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/rezygisk-tests.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT INT TERM
ORIG_PATH=$PATH
PASSED=0 FAILED=0 SKIPPED=0 CC_SKIPPED=0

ok() { PASSED=$((PASSED + 1)); echo "PASS $1"; }
bad() { FAILED=$((FAILED + 1)); echo "FAIL $1"; [ -z "${2:-}" ] || printf '%s\n' "$2" | sed 's/^/     /'; }
skip() { SKIPPED=$((SKIPPED + 1)); echo "SKIP $1 ($2)"; }
check() { name=$1; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }
have() { command -v "$1" >/dev/null 2>&1; }

# expect_eq <name> <expected> <actual>
expect_eq() {
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected: $2
actual:   $3"; fi
}

# wait_file <file>: background children of the scripts write their marker late
wait_file() {
  n=0
  while [ ! -s "$1" ] && [ "$n" -lt 50 ]; do sleep 0.1 2>/dev/null || sleep 1; n=$((n + 1)); done
  [ -s "$1" ]
}

# new_case <name>: fresh case dir $C with a stub dir $S and call log $L
new_case() {
  C="$W/$1"; S="$C/stubs"; L="$C/calls"
  mkdir -p "$C" "$S"; : > "$L"
}

# stub <name> [exit code]: records "<name> <args>" in $L
stub() {
  cat > "$S/$1" <<EOF
#!/bin/sh
echo "$1 \$*" >> "$L"
exit ${2:-0}
EOF
  chmod +x "$S/$1"
}

# getprop stub answering the two abilist properties from the environment
stub_getprop() {
  cat > "$S/getprop" <<'EOF'
#!/bin/sh
case "$1" in
  ro.system.product.cpu.abilist) printf '%s\n' "${TEST_ABILIST_SYSTEM:-}" ;;
  ro.product.cpu.abilist) printf '%s\n' "${TEST_ABILIST:-}" ;;
esac
EOF
  chmod +x "$S/getprop"
}

# which stub: finds magisk only when TEST_MAGISK=1
stub_which() {
  cat > "$S/which" <<'EOF'
#!/bin/sh
[ "$1" = magisk ] && [ "${TEST_MAGISK:-}" = 1 ] && { echo /sbin/magisk; exit 0; }
exit 1
EOF
  chmod +x "$S/which"
}

echo "== syntax"
for f in "$R"/module/src/*.sh "$R"/module/src/META-INF/com/google/android/update-binary "$R"/tests/*.sh; do
  rel=${f#"$R"/}
  if out=$(sh -n "$f" 2>&1); then ok "sh -n $rel"; else bad "sh -n $rel" "$out"; fi
done
if have bash; then
  if out=$(bash -n "$R/.forgejo/module-ci.sh" 2>&1); then ok "bash -n .forgejo/module-ci.sh"; else bad "bash -n .forgejo/module-ci.sh" "$out"; fi
else
  skip "bash -n .forgejo/module-ci.sh" "no bash"
fi

if have python3; then
  if out=$(python3 -I -c 'import py_compile,sys;py_compile.compile(sys.argv[1],cfile=sys.argv[2],doraise=True)' "$R/scripts/sign.py" "$W/sign.pyc" 2>&1); then ok "py_compile scripts/sign.py"; else bad "py_compile scripts/sign.py" "$out"; fi
  for f in "$R"/.forgejo/update.json "$R"/webroot/lang/*.json; do
    rel=${f#"$R"/}
    if out=$(python3 -I -m json.tool "$f" 2>&1 >/dev/null); then ok "json $rel"; else bad "json $rel" "$out"; fi
  done
  if python3 -I -c 'import yaml' 2>/dev/null; then
    for f in "$R"/.forgejo/workflows/*.yml "$R"/.github/workflows/*.yml; do
      rel=${f#"$R"/}
      if out=$(python3 -I -c 'import sys,yaml;yaml.safe_load(open(sys.argv[1]))' "$f" 2>&1); then ok "yaml $rel"; else bad "yaml $rel" "$out"; fi
    done
  else
    skip "yaml workflows" "no python3 yaml module"
  fi
else
  skip "py_compile scripts/sign.py" "no python3"
  skip "json parse" "no python3"
  skip "yaml workflows" "no python3"
fi

echo "== release workflow"
# The build step runs upstream-merged build code: it gets no signing secrets.
WF="$R/.forgejo/workflows/module-release.yml"
S="SIGN""_"
if grep -q "$S" "$WF"; then bad "workflow has no $S entries" "$(grep -n "$S" "$WF")"; else ok "workflow has no $S entries"; fi
if (cd "$R" && find . \( -path ./.git -o -path ./tests \) -prune -o -type f -exec grep -l "$S" {} + | grep -v '^./.forgejo/module-ci.sh$' | grep -q .); then
  bad "only module-ci.sh mentions $S outside tests/" "$(cd "$R" && find . \( -path ./.git -o -path ./tests \) -prune -o -type f -exec grep -l "$S" {} +)"
else
  ok "only module-ci.sh mentions $S outside tests/"
fi

echo "== README dependencies"
# The build tools the Makefile runs and the third-party modules sign.py
# imports must be listed in README.md's Dependencies section.
deps=$(sed -n '/^## Dependencies/,/^## Installation/p' "$R/README.md")
for tool in python3 zip; do
  grep -qE "(^|[[:space:]])$tool " "$R/Makefile" || { bad "Makefile still runs $tool"; continue; }
  if printf '%s\n' "$deps" | grep -q "\`$tool\`"; then ok "README lists $tool (used by the Makefile)"; else bad "README lists $tool (used by the Makefile)"; fi
done
for mod in $(sed -n 's/^from \([A-Za-z0-9_]*\).*/\1/p; s/^import \([A-Za-z0-9_]*\).*/\1/p' "$R/scripts/sign.py" | sort -u); do
  case "$mod" in sys|os|struct|hashlib|pathlib) continue ;; esac
  if printf '%s\n' "$deps" | grep -q "\`$mod\`"; then ok "README lists $mod (imported by sign.py)"; else bad "README lists $mod (imported by sign.py)"; fi
done

echo "== post-fs-data.sh"
# pfd_case <name>: module tree $C/modules/rezygisk with fake ptracer binaries
pfd_case() {
  new_case "$1"
  M="$C/modules/rezygisk"
  mkdir -p "$M/bin"
  cp "$R/module/src/post-fs-data.sh" "$M/"
  printf 'id=rezygisk\ndescription=Standalone implementation of Zygisk.\n' > "$M/module.prop.bak"
  printf 'id=rezygisk\ndescription=[old status] Standalone implementation of Zygisk.\n' > "$M/module.prop"
  for b in zygisk-ptrace64 zygisk-ptrace32; do
    printf '#!/bin/sh\necho "%s $*" > "%s/launched"\n' "$b" "$C" > "$M/bin/$b"
    chmod +x "$M/bin/$b"
  done
  for c in rm mkdir chmod chcon log; do stub "$c"; done
  stub_getprop; stub_which
}

# run_* <VAR=value>...: the assignments are exported to that run only
run_pfd() { (cd "$C" && for a in "$@"; do export "$a"; done && PATH="$S:$ORIG_PATH" sh "$M/post-fs-data.sh") > "$C/out" 2>&1; }

pfd_case pfd64
run_pfd TEST_ABILIST_SYSTEM=arm64-v8a,armeabi-v7a,armeabi TEST_ABILIST=arm64-v8a
expect_eq "post-fs-data: exit code" 0 "$?"
wait_file "$C/launched"
expect_eq "post-fs-data: 64-bit abilist starts the 64-bit monitor" "zygisk-ptrace64 monitor" "$(cat "$C/launched" 2>/dev/null)"
expect_eq "post-fs-data: recreates the runtime dir" "rm -rf /data/adb/rezygisk
mkdir -p /data/adb/rezygisk
chmod 555 /data/adb/rezygisk
chcon u:object_r:system_file:s0 /data/adb/rezygisk" "$(cat "$L")"
check "post-fs-data: module.prop restored from module.prop.bak" cmp -s "$M/module.prop.bak" "$M/module.prop"

pfd_case pfd32
run_pfd TEST_ABILIST_SYSTEM=armeabi-v7a,armeabi TEST_ABILIST=armeabi-v7a
wait_file "$C/launched"
expect_eq "post-fs-data: 32-bit only abilist starts the 32-bit monitor" "zygisk-ptrace32 monitor" "$(cat "$C/launched" 2>/dev/null)"

pfd_case pfdlonger
run_pfd TEST_ABILIST_SYSTEM=armeabi-v7a TEST_ABILIST=x86_64,x86,armeabi-v7a
wait_file "$C/launched"
expect_eq "post-fs-data: the longer abilist property wins" "zygisk-ptrace64 monitor" "$(cat "$C/launched" 2>/dev/null)"

pfd_case pfdzygisk
run_pfd ZYGISK_ENABLED=1 TEST_ABILIST=arm64-v8a
expect_eq "post-fs-data: Magisk Zygisk enabled exits 0" 0 "$?"
expect_eq "post-fs-data: Magisk Zygisk enabled touches nothing" "" "$(cat "$L")"
check "post-fs-data: Magisk Zygisk enabled starts no monitor" test ! -e "$C/launched"

pfd_case pfdmagisk
for m in enabled disabled nozygisk; do
  mkdir -p "$C/modules/$m"
  printf '#!/bin/sh\necho ran > "%s/%s.ran"\n' "$C" "$m" > "$C/modules/$m/post-fs-data.sh"
done
mkdir -p "$C/modules/enabled/zygisk" "$C/modules/disabled/zygisk"
: > "$C/modules/disabled/disable"
run_pfd TEST_MAGISK=1 TEST_ABILIST=arm64-v8a
expect_eq "post-fs-data: Magisk exit code" 0 "$?"
check "post-fs-data: Magisk runs post-fs-data.sh of enabled zygisk modules" test -s "$C/enabled.ran"
check "post-fs-data: Magisk skips disabled and non-zygisk modules" test ! -e "$C/disabled.ran" -a ! -e "$C/nozygisk.ran"

# a module script still running at the limit is left running in the
# background and the next module and the monitor are not held up
pfd_case pfdslow
for m in aslow bfast; do mkdir -p "$C/modules/$m/zygisk"; done
printf '#!/bin/sh\nsleep 3\necho ran > "%s/aslow.ran"\n' "$C" > "$C/modules/aslow/post-fs-data.sh"
printf '#!/bin/sh\necho ran > "%s/bfast.ran"\n' "$C" > "$C/modules/bfast/post-fs-data.sh"
run_pfd TEST_MAGISK=1 TEST_ABILIST=arm64-v8a REZYGISK_TEST_SCRIPT_LIMIT=1
expect_eq "post-fs-data: slow module script exit code" 0 "$?"
check "post-fs-data: a slow module script does not hold up the next module" test -s "$C/bfast.ran"
check "post-fs-data: a slow module script is left running, not waited for" test ! -e "$C/aslow.ran"
wait_file "$C/launched"
check "post-fs-data: the monitor starts while a slow module script runs" test -s "$C/launched"
n=0; while [ ! -s "$C/aslow.ran" ] && [ "$n" -lt 50 ]; do sleep 0.1 2>/dev/null || sleep 1; n=$((n + 1)); done
check "post-fs-data: the slow module script is never killed and completes" test -s "$C/aslow.ran"

echo "== service.sh"
svc_case() {
  new_case "$1"
  M="$C/modules/rezygisk"
  mkdir -p "$M"
  cp "$R/module/src/service.sh" "$M/"
  printf 'id=rezygisk\ndescription=Standalone implementation of Zygisk.\n' > "$M/module.prop.bak"
  cp "$M/module.prop.bak" "$M/module.prop"
  stub log; stub_which
}
run_svc() { (cd "$C" && for a in "$@"; do export "$a"; done && PATH="$S:$ORIG_PATH" sh "$M/service.sh") > "$C/out" 2>&1; }

svc_case svczygisk
run_svc ZYGISK_ENABLED=1
expect_eq "service: Magisk Zygisk enabled exits 0" 0 "$?"
expect_eq "service: Magisk Zygisk enabled marks the description" "description=[❌ Disable Magisk's built-in Zygisk] Standalone implementation of Zygisk." "$(grep '^description=' "$M/module.prop")"

svc_case svcmagisk
for m in enabled disabled; do
  mkdir -p "$C/modules/$m/zygisk"
  printf '#!/bin/sh\necho ran > "%s/%s.ran"\n' "$C" "$m" > "$C/modules/$m/service.sh"
done
: > "$C/modules/disabled/disable"
run_svc TEST_MAGISK=1
expect_eq "service: Magisk exit code" 0 "$?"
wait_file "$C/enabled.ran"
check "service: Magisk runs service.sh of enabled zygisk modules" test -s "$C/enabled.ran"
check "service: Magisk skips disabled modules" test ! -e "$C/disabled.ran"

echo "== uninstall.sh"
new_case uninstall
stub rm; stub rmdir
(PATH="$S:$ORIG_PATH" sh "$R/module/src/uninstall.sh") > "$C/out" 2>&1
expect_eq "uninstall: exit code" 0 "$?"
expect_eq "uninstall: removes runtime dir and service.d hook" "rm -rf /data/adb/rezygisk
rm -f /data/adb/service.d/rezygisk.sh
rmdir /data/adb/service.d" "$(cat "$L")"

new_case uninstallbusy
stub rm; stub rmdir 1
(PATH="$S:$ORIG_PATH" sh "$R/module/src/uninstall.sh") > "$C/out" 2>&1
expect_eq "uninstall: service.d still holding other scripts exits 0" 0 "$?"

echo "== rezygisk.sh"
if [ -e /data/adb/modules/rezygisk/disable ]; then
  skip "rezygisk.sh: enabled module" "host has /data/adb/modules/rezygisk/disable"
else
  new_case servicedhook
  stub cp
  (PATH="$S:$ORIG_PATH" sh "$R/module/src/rezygisk.sh") > "$C/out" 2>&1
  expect_eq "rezygisk.sh: enabled module exits 0" 0 "$?"
  expect_eq "rezygisk.sh: enabled module leaves module.prop alone" "" "$(cat "$L")"
fi

echo "== verify.sh"
# A directory stands in for the zip; the unzip stub copies matching entries out
# of it (-j junks paths, -x excludes) like unzip does.
make_unzip_stub() {
  cat > "$S/unzip" <<'EOF'
#!/bin/sh
junk=false dir=. zip= pats= excl=
while [ $# -gt 0 ]; do
  case "$1" in
    -o) ;;
    -oj|-jo) junk=true ;;
    -d) dir=$2; shift ;;
    -x) excl=$2; shift ;;
    *) if [ -z "$zip" ]; then zip=$1; else pats="$pats
$1"; fi ;;
  esac
  shift
done
cd "$zip" || exit 9
find . -type f | sed 's|^\./||' | while read -r f; do
  printf '%s\n' "$pats" | while read -r p; do
    [ -n "$p" ] || continue
    case "$f" in $p) ;; *) continue ;; esac
    if [ -n "$excl" ]; then case "$f" in $excl) continue ;; esac; fi
    if [ "$junk" = true ]; then t="$dir/${f##*/}"; else t="$dir/$f"; fi
    mkdir -p "${t%/*}" && cp "$f" "$t"
  done
done
exit 0
EOF
  chmod +x "$S/unzip"
}

# busybox sha256sum takes -s, coreutils spells it --status
make_sha256sum_stub() {
  real=$(PATH=$ORIG_PATH command -v sha256sum)
  cat > "$S/sha256sum" <<EOF
#!/bin/sh
if "$real" --version 2>/dev/null | grep -q coreutils; then
  for a in "\$@"; do shift; [ "\$a" = -s ] && a=--status; set -- "\$@" "\$a"; done
fi
exec "$real" "\$@"
EOF
  chmod +x "$S/sha256sum"
}

# zip_tree <dir>: module zip stand-in with a .sha256 sidecar per file
zip_tree() {
  Z=$1
  mkdir -p "$Z/META-INF/com/google/android" "$Z/bin/arm64-v8a"
  cp "$R/module/src/verify.sh" "$R/module/src/post-fs-data.sh" "$R/module/src/sepolicy.rule" "$Z/"
  cp "$R/module/src/META-INF/com/google/android/update-binary" "$Z/META-INF/com/google/android/"
  printf 'fake zygiskd\n' > "$Z/bin/arm64-v8a/zygiskd"
  (cd "$Z" && find . -type f | while read -r f; do printf '%s' "$(sha256sum "$f" | cut -d' ' -f1)" > "$f.sha256"; done)
}

verify_case() {
  new_case "$1"
  make_unzip_stub; make_sha256sum_stub
  zip_tree "$C/zip"
  mkdir -p "$C/tmp" "$C/mod/bin"
}

# run_extract <args for extract>: sources verify.sh like customize.sh does
run_extract() {
  (
    PATH="$S:$ORIG_PATH" TMPDIR="$C/tmp" ZIPFILE="$C/zip"
    ui_print() { echo "$1"; }
    abort() { echo "ABORT $1"; exit 1; }
    . "$C/zip/verify.sh"
    extract "$@"
  ) > "$C/out" 2>&1
}

verify_case vgood
run_extract "$C/zip" post-fs-data.sh "$C/mod"
expect_eq "verify: intact file exits 0" 0 "$?"
check "verify: intact file is extracted" cmp -s "$C/zip/post-fs-data.sh" "$C/mod/post-fs-data.sh"
check "verify: intact file is reported verified" grep -q '^- Verified post-fs-data.sh$' "$C/out"
check "verify: update-binary checked" grep -q '^- Verified META-INF/com/google/android/update-binary$' "$C/out"

verify_case vjunk
run_extract "$C/zip" bin/arm64-v8a/zygiskd "$C/mod/bin" true
expect_eq "verify: junk-path extract exits 0" 0 "$?"
check "verify: junk-path extract lands in the target dir" test -f "$C/mod/bin/zygiskd"

verify_case vtamper
echo 'tampered' >> "$C/zip/post-fs-data.sh"
run_extract "$C/zip" post-fs-data.sh "$C/mod"
expect_eq "verify: tampered file aborts" 1 "$?"
check "verify: tampered file abort message" grep -q '^! Failed to verify post-fs-data.sh$' "$C/out"

verify_case vnosidecar
rm "$C/zip/sepolicy.rule.sha256"
run_extract "$C/zip" sepolicy.rule "$C/mod"
expect_eq "verify: missing sidecar aborts" 1 "$?"
check "verify: missing sidecar abort message" grep -q '^! sepolicy.rule.sha256 not exists$' "$C/out"

echo "== customize.sh version gates"
# The early checks of customize.sh run before it touches any file; placeholders
# get the values of common.mk like the Makefile does.
cust_case() {
  new_case "$1"
  sed -e 's/@DEBUG@/false/g' -e 's/@MIN_APATCH_VERSION@/10655/g' -e 's/@MIN_KSU_VERSION@/10940/g' \
      -e 's/@MIN_KSUD_VERSION@/11425/g' -e 's/@MIN_MAGISK_VERSION@/26402/g' \
      "$R/module/src/customize.sh" > "$C/customize.sh"
  stub_which
  mkdir -p "$C/tmp"
  printf 'version=v0.0.0-test\n' > "$C/tmp/module.prop"
}
run_cust() {
  (
    for a in "$@"; do export "$a"; done
    PATH="$S:$ORIG_PATH" TMPDIR="$C/tmp"
    ui_print() { echo "$1"; }
    abort() { echo "ABORT $1"; exit 1; }
    grep_prop() { sed -n "s/^$1=//p" "${2:-/dev/null}" | head -n 1; }
    unzip() { echo "UNZIP $*"; exit 3; }
    . "$C/customize.sh"
  ) > "$C/out" 2>&1
}
# expect_abort <name> <message line>
expect_abort() {
  rc=$?
  if [ "$rc" = 1 ] && grep -qF -- "$2" "$C/out"; then ok "$1"; else bad "$1" "rc=$rc; $(cat "$C/out")"; fi
}

cust_case crecovery
run_cust
expect_abort "customize: recovery install refused" "! Install from recovery is not supported"

cust_case cksuold
run_cust BOOTMODE=true KSU=true KSU_KERNEL_VER_CODE=10939 KSU_VER_CODE=11425
expect_abort "customize: old KernelSU refused" "! KernelSU version is too old!"

cust_case cksudold
run_cust BOOTMODE=true KSU=true KSU_KERNEL_VER_CODE=10940 KSU_VER_CODE=11424
expect_abort "customize: old ksud refused" "! ksud version is too old!"

cust_case cksumagisk
run_cust BOOTMODE=true KSU=true KSU_KERNEL_VER_CODE=10940 KSU_VER_CODE=11425 TEST_MAGISK=1
expect_abort "customize: KernelSU plus Magisk refused" "! Multiple root implementation is NOT supported!"

cust_case capatchold
run_cust BOOTMODE=true APATCH=true APATCH_VER_CODE=10654
expect_abort "customize: old APatch refused" "! APatch version is too old!"

cust_case cmagiskold
run_cust BOOTMODE=true MAGISK_VER_CODE=26401
expect_abort "customize: old Magisk refused" "! Magisk version is too old!"

cust_case capi
run_cust BOOTMODE=true MAGISK_VER_CODE=26402 API=24 ARCH=arm64
expect_abort "customize: SDK 24 refused" "! Minimal supported sdk is 25 (Android 7.1)"

cust_case carch
run_cust BOOTMODE=true MAGISK_VER_CODE=26402 API=34 ARCH=riscv64
expect_abort "customize: unsupported arch refused" "! Unsupported platform: riscv64"

cust_case cpass
run_cust BOOTMODE=true MAGISK_VER_CODE=26402 API=34 ARCH=arm64
rc=$?
if [ "$rc" = 3 ] && grep -q '^- Device platform: arm64$' "$C/out" && grep -q '^UNZIP -o .* verify.sh' "$C/out"; then
  ok "customize: supported Magisk device passes the gates"
else
  bad "customize: supported Magisk device passes the gates" "rc=$rc; $(cat "$C/out")"
fi

echo "== C (host compiler)"
# Each tests/c/*_test.c includes the source it tests; device-only calls are
# redirected there. c_build compiles once, c_test runs one named case.
CC_BIN=${CC:-cc}
c_build() {
  bin="$W/$1"; shift
  if ! have "$CC_BIN"; then return 1; fi
  if ! out=$("$CC_BIN" -std=gnu2x -D_GNU_SOURCE -w -I"$R/tests/c/include" -I"$R/zygiskd/src" -I"$R/zygiskd/src/root_impl" \
       -I"$R/loader/src/include" -o "$bin" "$@" -lpthread 2>&1); then
    printf '%s\n' "$out" > "$bin.err"
  fi
  return 0
}
# c_test <binary> <case> <description>
c_test() {
  if ! have "$CC_BIN"; then
    CC_SKIPPED=$((CC_SKIPPED + 1)); skip "$3" "no C compiler $CC_BIN"; return
  fi
  if [ ! -x "$W/$1" ]; then bad "$3" "compile failed: $(head -n 20 "$W/$1.err" 2>/dev/null)"; return; fi
  if out=$(cd "$W" && TEST_DIR="$W/$1.d" "$W/$1" "$2" 2>&1); then ok "$3"; else bad "$3" "$(printf '%s\n' "$out" | tail -n 5)"; fi
}

c_build zygiskd_utils "$R/tests/c/zygiskd_utils_test.c"
c_test zygiskd_utils read_string_value "zygiskd read_string: reads a string"
c_test zygiskd_utils read_string_empty "zygiskd read_string: empty string is NUL-terminated"
c_test zygiskd_utils current_attr_bounds "zygiskd get_current_attr: stays inside the buffer"
c_test zygiskd_utils sockcreate_no_leak "zygiskd set_socket_create_context: failed write closes the file"
c_test zygiskd_utils clean_ns_unshare_fails "zygiskd save_mns_fd: failed unshare unmounts nothing"
c_test zygiskd_utils clean_ns_failure_reaps_child "zygiskd save_mns_fd: failure reaps the helper child"
c_test zygiskd_utils mns_unknown_state_refused "zygiskd save_mns_fd: an unknown namespace state is refused (no fd left open)"
c_test zygiskd_utils saved_ns_fd_cloexec "zygiskd save_mns_fd: the saved namespace fd is closed on exec"
c_test zygiskd_utils mounted_ns_logs_no_error "zygiskd save_mns_fd: success logs no error"

# helpers with a 200 ms ZYGISKD_PEER_TIMEOUT_MS
c_build zygiskd_utils_timeout "$R/tests/c/zygiskd_utils_test.c" -DZYGISKD_PEER_TIMEOUT_MS=200
c_test zygiskd_utils_timeout exec_output "zygiskd exec_command: returns the output"
c_test zygiskd_utils_timeout exec_split_output "zygiskd exec_command: output written in parts is read whole"
c_test zygiskd_utils_timeout exec_child_hangs "zygiskd exec_command: a hanging command is killed and fails after the time limit"
c_test zygiskd_utils_timeout exec_child_hangs_after_closing "zygiskd exec_command: a command hanging after closing its output is killed too"
c_test zygiskd_utils_timeout clean_ns_helper_hangs "zygiskd save_mns_fd: a namespace helper stuck in umount is killed and fails in time"
c_test zygiskd_utils_timeout datagram_queue_full "zygiskd: a full monitor queue does not block sending it a datagram"

c_build zygiskd_common "$R/tests/c/zygiskd_common_test.c"
c_test zygiskd_common uid_from_pkg_found "zygiskd uid_from_pkg: installed package gives its app id"
c_test zygiskd_common uid_from_pkg_missing "zygiskd uid_from_pkg: missing package gives 0"

c_build zygiskd_daemon "$R/tests/c/zygiskd_daemon_test.c" "$R/zygiskd/src/utils.c"
c_test zygiskd_daemon survives_empty_client "zygiskd: a client leaving without a request does not stop the daemon"
c_test zygiskd_daemon accept_econnaborted "zygiskd: accept() ECONNABORTED does not stop the daemon"
c_test zygiskd_daemon accept_eintr "zygiskd: accept() EINTR does not stop the daemon"
c_test zygiskd_daemon accept_emfile_exits "zygiskd: accept() EMFILE still ends the daemon (no fd is freed while it retries)"
c_test zygiskd_daemon accept_enfile_exits "zygiskd: accept() ENFILE still ends the daemon (no busy retry loop)"
c_test zygiskd_daemon accept_einval_exits "zygiskd: other accept() errors still end the daemon"
c_test zygiskd_daemon accept_econnaborted_log_fails "zygiskd: accept() errno is judged before logging can change it"

# same daemon with a 300 ms peer timeout (ZYGISKD_PEER_TIMEOUT_MS)
c_build zygiskd_daemon_timeout "$R/tests/c/zygiskd_daemon_test.c" "$R/zygiskd/src/utils.c" -DZYGISKD_PEER_TIMEOUT_MS=300
c_test zygiskd_daemon_timeout stalled_client_before_request "zygiskd: a client stuck before its request is dropped after the timeout"
c_test zygiskd_daemon_timeout stalled_client_mid_request "zygiskd: a client stuck in the middle of a request is dropped after the timeout"
c_test zygiskd_daemon_timeout companion_channel_untimed "zygiskd: the client fd handed to a companion carries no timeout"
c_test zygiskd_daemon_timeout companion_spawn_hangs "zygiskd: a companion spawn that never finishes fails the request in time"
c_test zygiskd_daemon_timeout companion_handshake_stalls "zygiskd: a companion that never answers its handshake fails the request in time"
c_test zygiskd_daemon_timeout companion_inherits_no_fds "zygiskd: a module companion inherits no zygiskd fd but its own channel"
c_test zygiskd_daemon_timeout gone_client_remove_module "zygiskd: a RemoveModule whose client already left is not carried out"
c_test zygiskd_daemon_timeout gone_client_process_flags "zygiskd: a flags request whose client already left does not use up the first-process flag"

c_build zygiskd_companion "$R/tests/c/zygiskd_companion_test.c" "$R/zygiskd/src/utils.c"
c_test zygiskd_companion survives_gone_client "zygiskd companion: a client gone before its ack does not end the companion"
c_build zygiskd_companion_timeout "$R/tests/c/zygiskd_companion_test.c" "$R/zygiskd/src/utils.c" -DZYGISKD_PEER_TIMEOUT_MS=200
c_test zygiskd_companion_timeout handshake_never_comes "zygiskd companion: a handshake that never comes ends the companion in time"
c_test zygiskd_companion_timeout idle_longer_than_limit "zygiskd companion: idling longer than the limit still serves the next client"

c_build zygiskd_rootimpl "$R/tests/c/zygiskd_rootimpl_test.c"
c_test zygiskd_rootimpl apatch_version "zygiskd APatch: version parsed from apd -V"
c_test zygiskd_rootimpl apatch_empty_output "zygiskd APatch: empty apd -V output is abnormal"
c_test zygiskd_rootimpl magisk_requester "zygiskd Magisk: manager package from the requester string"
c_test zygiskd_rootimpl magisk_short_output "zygiskd Magisk: output without value= keeps the default manager"
c_test zygiskd_rootimpl magisk_sql_plain "zygiskd Magisk: denylist query for a plain process name"
c_test zygiskd_rootimpl magisk_sql_quotes "zygiskd Magisk: quotes in the process name stay inside the SQL literal"
c_test zygiskd_rootimpl magisk_sql_max_quotes "zygiskd Magisk: longest all-quote process name gives a complete query"

c_build monitor "$R/tests/c/monitor_test.c"
c_test monitor state_json_stop_reason "monitor: state.json with a stop reason is valid JSON"
c_test monitor listener_stale_path "monitor: stale init_monitor socket path does not block the bind"
c_test monitor listener_bind_fail_closes "monitor: failed bind closes the socket"
c_test monitor sections_bounded "monitor: oversized module.prop stays inside the section buffers"
c_test monitor sections_exact_fit "monitor: lines after the description filling the buffer are never cut"
c_test monitor pre_section_keeps_key "monitor: lines before the description never push out the description= key"
c_test monitor prop_rewrite_exact "monitor: status rewrite changes only the description, keeps updateJson and mode"
c_test monitor prop_write_failure_keeps_old "monitor: failed module.prop write leaves the old file intact"
c_test monitor status_text_bounded "monitor: long daemon error text stays inside the status text"
c_test monitor epoll_fd_cloexec "monitor: the epoll fd is closed on exec (not inherited by ReZygiskd and the tracers)"
c_test monitor status_text_utf8_whole "monitor: a status text cut at its limit never splits a UTF-8 character"
c_test monitor description_utf8_whole "monitor: a description shortened for the lines after it never splits a UTF-8 character"
c_test monitor state_json_escapes "monitor: strings in state.json are JSON-escaped"
c_test monitor state_json_write_failure_keeps_old "monitor: failed state.json write leaves the old file intact"
c_test monitor sigchld_echild_tracing "monitor: SIGCHLD with no child left does not spin on waitpid"
c_test monitor daemon_exit_updates_status "monitor: ReZygiskd exit is shown in module.prop at once"
c_test monitor auto_stop_updates_status "monitor: an automatic stop is shown in module.prop once init is released"
c_test monitor status_no_line_injection "monitor: line breaks in the daemon error text add no module.prop lines"
c_test monitor info_module_count_bounded "monitor: module count larger than the datagram allocates nothing for it"
c_test monitor ctl_start_seizes "monitor: ctl start seizes init and traces"
c_test monitor ctl_start_seize_fails "monitor: ctl start stays stopped when init cannot be seized"
c_test monitor exec_stop_unknown_program "monitor: a process at its exec stop whose program cannot be read is detached, not left stopped"

# monitor waits with a short REZYGISK_TRACE_TIMEOUT_MS
c_build monitor_timeout "$R/tests/c/monitor_test.c" -DREZYGISK_TRACE_TIMEOUT_MS=200
c_test monitor_timeout stop_wait_times_out "monitor: an app_process that never reports its stop is resumed, not killed or left stopped"
c_test monitor_timeout ctl_queue_full "zygisk-ptrace ctl: a monitor whose queue is full fails the command in time"

c_build ptracer_main "$R/tests/c/ptracer_main_test.c"
c_test ptracer_main trace_valid_pid "zygisk-ptrace trace: a valid pid is traced"
c_test ptracer_main trace_non_numeric_pid "zygisk-ptrace trace: non-numeric pid rejected before anything runs"
c_test ptracer_main trace_trailing_garbage_pid "zygisk-ptrace trace: pid with trailing garbage rejected"
c_test ptracer_main trace_zero_pid "zygisk-ptrace trace: pid 0 rejected"
c_test ptracer_main trace_negative_pid "zygisk-ptrace trace: negative pid rejected"
c_test ptracer_main ctl_without_command "zygisk-ptrace ctl: missing command prints usage and exits 1"

c_build ptracer_utils "$R/tests/c/ptracer_utils_test.c"
c_test ptracer_utils fork_ok "zygisk-ptrace fork_dont_care: grandchild gets 0, caller a pid"
c_test ptracer_utils fork_ok_sigchld_ignored "zygisk-ptrace fork_dont_care: SIGCHLD ignored still gives the caller a pid"
c_test ptracer_utils second_fork_fails "zygisk-ptrace fork_dont_care: failed second fork returns -1 to the caller only"

# tracer waits with short REZYGISK_TRACE_TIMEOUT_MS / REZYGISK_REMOTE_CALL_TIMEOUT_MS
c_build ptracer_utils_timeout "$R/tests/c/ptracer_utils_test.c" -DREZYGISK_TRACE_TIMEOUT_MS=200 -DREZYGISK_REMOTE_CALL_TIMEOUT_MS=400
c_test ptracer_utils_timeout trace_wait_times_out "zygisk-ptrace wait_for_trace: a tracee that never stops ends the wait like a failed wait"
c_test ptracer_utils_timeout syscall_stop_times_out "zygisk-ptrace wait_for_ptrace_syscall_stop: a tracee that never stops fails the wait"
c_test ptracer_utils_timeout remote_call_times_out "zygisk-ptrace remote_call: a remote function that never returns fails the call"
c_test ptracer_utils_timeout linker_never_ready "zygisk-ptrace wait_linker_ready: a linker that never resolves gives up in time"
c_test ptracer_utils_timeout fork_reap_times_out "zygisk-ptrace fork_dont_care: an intermediate child never reported still gives the caller a pid"

c_build loader_misc "$R/tests/c/loader_misc_test.c"
c_test loader_misc maps_parsed "loader parse_maps: entries parsed and freed"
c_test loader_misc empty_maps_no_leak "loader parse_maps: no mapping found leaves nothing allocated"
c_test loader_misc last_line_without_newline "loader parse_maps: last line without a newline keeps its path"

c_build ptracer_trace "$R/tests/c/ptracer_trace_test.c"
c_test ptracer_trace sigcont_detaches "zygisk-ptrace trace_zygote: SIGCONT after injection detaches and succeeds"
c_test ptracer_trace unexpected_stop_detaches "zygisk-ptrace trace_zygote: another stop after injection detaches and keeps the injected zygote"
c_test ptracer_trace seize_wait_times_out "zygisk-ptrace trace_zygote: a timed-out wait after seize fails at once"
c_test ptracer_trace group_stop_wait_times_out "zygisk-ptrace trace_zygote: a timed-out wait after injection fails"
c_test ptracer_trace sigcont_wait_times_out "zygisk-ptrace trace_zygote: a timed-out wait for SIGCONT fails instead of claiming a detach"
c_test ptracer_trace final_wait_times_out "zygisk-ptrace trace_zygote: a timed-out last wait fails instead of claiming a detach"

c_build remote_csoloader "$R/tests/c/remote_csoloader_test.c" "$R/loader/src/common/socket_utils.c"
c_test remote_csoloader short_phdr_read "remote_csoloader: program headers cut by the end of the file are refused"
c_test remote_csoloader reserve_mmap_errno "remote_csoloader: a remote mmap reserve returning -errno stops the load"
c_test remote_csoloader segment_mmap_errno "remote_csoloader: a remote segment mmap returning -errno stops the load"

# hook.c builds against the test-only tests/c/include/jni.h and the
# headers of the csoloader and plti submodules
if [ -f "$R/loader/src/external/csoloader/include/csoloader.h" ] && [ -f "$R/loader/src/external/plti/src/plti.h" ]; then
  c_build hook "$R/tests/c/hook_test.c" -I"$R/loader/src/injector" -I"$R/loader/src/external/csoloader/include" -I"$R/loader/src/external/plti/src"
  c_test hook remove_failed_modules "loader: modules that fail to load are removed from zygiskd by their current index"
  c_test hook unregistered_module_skipped "loader: a module that never registers has no specialize hooks called (no stale table memory)"
  c_test hook isolated_without_data_dir "loader: an isolated service without an app data dir is specialized without reading a null string"
  c_test hook fork_parent_releases_name "loader: zygote releases the process name of every app it forks"
  c_test hook jni_table_full "loader: a JNI method beyond the 32-entry hook table is reported as not hooked"
  c_test hook register_table_race "loader: plt hook registration cannot overflow its table between check and lock"
  c_test hook exclude_table_race "loader: plt hook exclusion cannot overflow its table between check and lock"
else
  CC_SKIPPED=$((CC_SKIPPED + 7))
  skip "loader: modules that fail to load are removed from zygiskd by their current index" "submodules not checked out (git submodule update --init)"
  skip "loader: a module that never registers has no specialize hooks called (no stale table memory)" "submodules not checked out (git submodule update --init)"
  skip "loader: an isolated service without an app data dir is specialized without reading a null string" "submodules not checked out (git submodule update --init)"
  skip "loader: zygote releases the process name of every app it forks" "submodules not checked out (git submodule update --init)"
  skip "loader: a JNI method beyond the 32-entry hook table is reported as not hooked" "submodules not checked out (git submodule update --init)"
  skip "loader: plt hook registration cannot overflow its table between check and lock" "submodules not checked out (git submodule update --init)"
  skip "loader: plt hook exclusion cannot overflow its table between check and lock" "submodules not checked out (git submodule update --init)"
fi

# loader requests with a 200 ms ZYGISKD_PEER_TIMEOUT_MS
c_build loader_timeout "$R/tests/c/loader_timeout_test.c" "$R/loader/src/common/socket_utils.c" -DZYGISKD_PEER_TIMEOUT_MS=200
c_test loader_timeout silent_peer "loader: a silent zygiskd fails the request like a closed one, after one timeout"
c_test loader_timeout first_field_then_silent "loader: a zygiskd going silent mid-answer fails the request after one timeout"
c_test loader_timeout backlog_full "loader: connect() to a zygiskd that never accepts gives up after one timeout"
c_test loader_timeout companion_fd_untimed "loader: the companion fd handed to a module carries no timeout"
c_test loader_timeout nonblocking_pipe_retries "loader: read_loop still retries EAGAIN on a non-blocking pipe"
c_test loader_timeout untimed_socket_waits "loader: read_loop still waits on a socket without a timeout"

c_build loader_daemon "$R/tests/c/loader_daemon_test.c" "$R/loader/src/common/socket_utils.c"
c_test loader_daemon get_info_daemon_down "loader rezygiskd_get_info: daemon down leaves an empty, freeable info"
c_test loader_daemon get_info_name_at_eof "loader rezygiskd_get_info: a module.prop ending with name= and no newline keeps the module list"
c_test loader_daemon get_info_name_too_long "loader rezygiskd_get_info: a name line longer than the line buffer is still refused"

c_build loader_socket "$R/tests/c/loader_socket_test.c"
c_test loader_socket read_string_value "loader read_string: reads a string"
c_test loader_socket read_string_huge_length "loader read_string: SIZE_MAX length is refused before allocating"
c_test loader_socket read_uint32_full "loader read_uint32_t: reads a full value"
c_test loader_socket read_uint32_short "loader read_uint32_t: short answer is a read failure"

echo "== WebUI (node)"
# tests/webui/webui_test.mjs imports the real webroot/js files with
# browser and KernelSU stand-ins
NODE_SKIPPED=0
node_test() {
  if ! have node; then
    NODE_SKIPPED=$((NODE_SKIPPED + 1)); skip "$2" "no node"; return
  fi
  d="$W/webui-$1"; mkdir -p "$d"
  if out=$(node "$R/tests/webui/webui_test.mjs" "$1" "$d" 2>&1); then ok "$2"; else bad "$2" "$(printf '%s\n' "$out" | tail -n 6)"; fi
}
node_test error_log_literal "WebUI: error text reaches the log file literally, never as shell code"
node_test error_log_no_error_object "WebUI: an error event without an error object is logged"
node_test rejection_log_literal "WebUI: rejection reason reaches the log file literally, never as shell code"
node_test module_ids_validated "WebUI: module ids outside the module id charset never reach the shell"
node_test credit_link_quoted "WebUI: a credit link reaches am start as one quoted word"
node_test module_names_in_order "WebUI: each module shows its own name, also next to modules without one"
node_test home_zygote64_not_injected "WebUI: a 64-bit zygote reported as not injected is shown as not injected"
node_test module_names_escaped "WebUI: module names are shown as text, not parsed as HTML"
node_test modules_state_unreadable "WebUI: the modules page survives an unreadable state.json"
node_test modules_names_unreadable "WebUI: the modules page lists modules when their names cannot be read"

echo "passed $PASSED failed $FAILED skipped $SKIPPED"
if [ "$FAILED" -ne 0 ]; then exit 1; fi
if [ "${REZYGISK_REQUIRE_NODE:-}" = 1 ] && [ "$NODE_SKIPPED" -ne 0 ]; then
  echo "REZYGISK_REQUIRE_NODE=1: $NODE_SKIPPED node case(s) skipped"
  exit 1
fi
if [ "${REZYGISK_REQUIRE_CC:-}" = 1 ] && [ "$CC_SKIPPED" -ne 0 ]; then
  echo "REZYGISK_REQUIRE_CC=1: $CC_SKIPPED C case(s) skipped"
  exit 1
fi
exit 0
