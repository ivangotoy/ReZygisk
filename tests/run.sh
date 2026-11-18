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

echo "passed $PASSED failed $FAILED skipped $SKIPPED"
if [ "$FAILED" -ne 0 ]; then exit 1; fi
if [ "${REZYGISK_REQUIRE_CC:-}" = 1 ] && [ "$CC_SKIPPED" -ne 0 ]; then
  echo "REZYGISK_REQUIRE_CC=1: $CC_SKIPPED C case(s) skipped"
  exit 1
fi
exit 0
