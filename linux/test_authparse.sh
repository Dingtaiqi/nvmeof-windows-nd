#!/bin/sh
# Exercise the key-file reading block of nvmet_setup.sh as an ORDINARY user.
#
# The whole script cannot run without root (it mounts configfs and modprobes), and
# the auth block sits after those, so the parsing would never be reached.  This
# pulls that block out verbatim by line number and drives it, which is the only way
# to prove the file-reading and rejection paths without a password.
set -e
SRC=/tmp/nvmet_setup.sh

start=$(grep -n '^AUTH_KEY="\${AUTH_KEY:-}"' "$SRC" | cut -d: -f1)
# The block ends where the configfs writes begin, NOT at the first `esac`: there are
# two case statements inside it (the "-|none" switch and the DHHC-1 check), and
# stopping at the first one silently dropped the file-reading code - which is the
# only part worth testing.  (The first version of this test did exactly that and
# "passed" two cases for the wrong reason.)
end=$(grep -n '^if \[ -n "\$AUTH_KEY" \]; then' "$SRC" | head -1 | cut -d: -f1)
end=$((end - 1))
echo "auth-parse block: lines $start..$end"
[ -n "$start" ] && [ -n "$end" ] && [ "$end" -gt "$start" ] || { echo "FAIL: block not found"; exit 1; }

head -"$end" "$SRC" | tail -n +"$start" > /tmp/authparse.sh
# Silence the "not defined" complaints from `set -u` style reads; the block only
# reads optional variables.
cat > /tmp/authparse_driver.sh <<'EOF'
#!/bin/sh
AUTH_KEY_FILE="$1"
set -e
. /tmp/authparse.sh
echo "RESULT key_len=${#AUTH_KEY} ctrl_len=${#AUTH_CTRL_KEY} keyfile=${AUTH_KEY_FILE:-<none>}"
EOF
chmod +x /tmp/authparse_driver.sh

pass=0; fail=0
check() { # name expected_substring actual
    case "$3" in
        *"$2"*) echo "  [ok] $1"; pass=$((pass+1)) ;;
        *)      echo "  [FAIL] $1 - got: $3"; fail=$((fail+1)) ;;
    esac
}

# 1. a good file: line 1 host key, line 2 controller key
printf 'DHHC-1:01:AAAA:\nDHHC-1:01:BBBB:\nDHHC-1:01:CCCC:\n' > /tmp/k_good
out=$(/tmp/authparse_driver.sh /tmp/k_good); check "two-line key file" "key_len=15 ctrl_len=15" "$out"
# 2. one line only: one-way
printf 'DHHC-1:01:AAAA:\n' > /tmp/k_one
out=$(/tmp/authparse_driver.sh /tmp/k_one); check "one-line key file" "key_len=15 ctrl_len=0" "$out"
# 3. a file that is not a key at all must be refused, loudly
printf 'not-a-key\n' > /tmp/k_bad
out=$(/tmp/authparse_driver.sh /tmp/k_bad 2>&1 || true)
check "a non-key file is refused" "does not start with DHHC-1" "$out"
# 4. a missing file must be refused
out=$(/tmp/authparse_driver.sh /tmp/k_missing 2>&1 || true)
check "a missing file is refused" "cannot read the key file" "$out"
# 5. "-" means explicitly off, and must not be treated as a path
out=$(/tmp/authparse_driver.sh - 2>&1 || true)
check "'-' means no authentication" "key_len=0 ctrl_len=0 keyfile=<none>" "$out"
# 6. no argument at all
out=$(/tmp/authparse_driver.sh "" 2>&1 || true)
check "no argument means no authentication" "key_len=0 ctrl_len=0" "$out"

echo "auth-parse: $pass ok, $fail failed"
[ "$fail" -eq 0 ]
