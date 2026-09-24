#!/bin/sh
# Host regression test for initramfs/usr/bin/afe-debug: the dynamic-debug
# toggler spk-protect-probe wraps its tone phases in. Runs the script as a
# real subprocess against a fake dynamic_debug control listing (the real
# "file:line [module]function =flags "fmt"" shape, with sibling sites that
# must NOT be touched), a stub mount that "creates" the control file, and a
# stub kernel-side writer (DYNDBG_WRITER) that applies each `func NAME +p/-p`
# query to the fake listing the way ddebug_exec_query would -- including the
# kernel's silent acceptance of a query that matches nothing, which the
# script must catch by re-reading the listing.
set -eu
cd "$(dirname "$0")/../.."
SCRIPT=initramfs/usr/bin/afe-debug
[ -r "$SCRIPT" ] || { echo "cannot find $SCRIPT" >&2; exit 1; }

TMPROOT=$(mktemp -d /tmp/afe-debug-test.XXXXXX)
trap 'rm -rf "$TMPROOT"' EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
eq() { if [ "$2" = "$3" ]; then ok; else bad "$1: expected '$2', got '$3'"; fi; }

# ---- the site list must match the kernel tree: every listed function has
# to own at least one pr_debug in q6afe.c or tas2560-algo.c, otherwise a
# real `afe-debug on` fails its own verification. ----
AFE_DEBUG_SELFTEST=1 . "./$SCRIPT"
for name in $SITES; do
	hits=$(awk -v fn="$name" '
		$0 ~ ("^(static )?[a-z0-9_ ]*[ *]" fn "\\(") { inf = 1 }
		inf && /pr_debug/ { c++ }
		inf && /^}/ { inf = 0 }
		END { print c + 0 }
	' kernel/sound/soc/msm/qdsp6v2/q6afe.c kernel/sound/soc/msm/tas2560-algo.c)
	if [ "$hits" -gt 0 ]; then ok; else bad "site $name has no pr_debug in the kernel tree (would fail verification live)"; fi
done
for must in afe_callback_debug_print afe_apr_send_pkt afe_send_port_topology_id tas2560_algo_afe_get_param __afe_port_start; do
	case " $(printf '%s ' $SITES)" in
	*" $must "*) ok ;;
	*) bad "site list must include $must" ;;
	esac
done

# ---- fixture: fake control listing + stub writer + stub mount ----
CONTROL="$TMPROOT/control"
write_listing() {
	# All sites off. Includes afe_callback AND afe_callback_debug_print so
	# whole-name matching is exercised, and an unrelated q6adm site that
	# must stay untouched.
	cat > "$CONTROL" <<'EOF'
sound/soc/msm/qdsp6v2/q6afe.c:286 [q6afe]afe_callback_debug_print =_ "%s: code = 0x%x PL#0[0x%x], PL#1[0x%x], size = %d\012"
sound/soc/msm/qdsp6v2/q6afe.c:290 [q6afe]afe_callback_debug_print =_ "%s: code = 0x%x PL#0[0x%x], size = %d\012"
sound/soc/msm/qdsp6v2/q6afe.c:547 [q6afe]afe_callback =_ "%s: reset event = %d %d apr[%pK]\012"
sound/soc/msm/qdsp6v2/q6afe.c:1111 [q6afe]afe_apr_send_pkt =_ "%s: DSP returned error[%s]\012"
sound/soc/msm/qdsp6v2/q6afe.c:3315 [q6afe]__afe_port_start =_ "%s: port id: 0x%x\012"
sound/soc/msm/qdsp6v2/q6afe.c:2048 [q6afe]afe_send_port_topology_id =_ "%s: AFE port[%d] get_cal_topology[%d] invalid!\012"
sound/soc/msm/qdsp6v2/q6afe.c:2077 [q6afe]afe_send_port_topology_id =_ "%s: AFE set topology id 0x%x  enable for port 0x%x ret %d\012"
sound/soc/msm/qdsp6v2/q6afe.c:2020 [q6afe]afe_get_cal_topology_id =_ "%s: port_id = %u acdb_id = %d topology_id = %u\012"
sound/soc/msm/qdsp6v2/q6afe.c:1960 [q6afe]afe_find_cal_topo_id_by_port =_ "%s: top_id:%x acdb_id:%d afe_port:%d\012"
sound/soc/msm/qdsp6v2/q6afe.c:1937 [q6afe]afe_send_hw_delay =_ "%s: port_id 0x%x rate %u delay_usec %d status %d\012"
sound/soc/msm/qdsp6v2/q6afe.c:1540 [q6afe]afe_send_cal_block =_ "%s: %d cal_block %pK\012"
sound/soc/msm/tas2560-algo.c:93 [tas2560_algo]tas2560_algo_afe_set_param =_ "TAS2560_ALGO:%s port %d failed with code %d\012"
sound/soc/msm/tas2560-algo.c:127 [tas2560_algo]tas2560_algo_afe_get_param =_ "TAS2560_ALGO:%s for port %d failed with code %d\012"
sound/soc/msm/qdsp6v2/q6adm.c:100 [q6adm]adm_callback =_ "%s: unrelated site must stay off\012"
EOF
}

STUBDIR="$TMPROOT/stubbin"
mkdir -p "$STUBDIR"
cat > "$STUBDIR/dyndbg-writer" <<'STUB'
#!/bin/sh
# Stub kernel: apply "func NAME +p|-p" to the fake listing in place.
# STUB_IGNORE_FUNC=NAME simulates the kernel silently matching nothing for
# that function (e.g. the function got renamed): the write "succeeds" but
# the listing is unchanged. STUB_WRITER_FAIL=1 makes the write itself fail.
echo "$1" >> "$WRITER_LOG"
[ "${STUB_WRITER_FAIL:-0}" = 1 ] && exit 1
set -- $1
[ "$1" = func ] || exit 1
name=$2
op=$3
[ "$name" = "${STUB_IGNORE_FUNC:-}" ] && exit 0
case "$op" in
+p) new="=p" ;;
-p) new="=_" ;;
*) exit 1 ;;
esac
awk -v name="$name" -v new="$new" '
	{
		f = $2
		sub(/^\[[^]]*\]/, "", f)
		if (f == name) $3 = new
		print
	}
' "$DYNDBG_CONTROL" > "$DYNDBG_CONTROL.new" && mv "$DYNDBG_CONTROL.new" "$DYNDBG_CONTROL"
STUB
cat > "$STUBDIR/mount" <<'STUB'
#!/bin/sh
echo "mount $*" >> "$WRITER_LOG"
# STUB_MOUNT_CREATES=1: a successful debugfs mount makes the control file
# appear (copy the prepared listing into place).
if [ "${STUB_MOUNT_CREATES:-0}" = 1 ]; then
	mkdir -p "$(dirname "$DYNDBG_CONTROL")"
	cp "$PREPARED_CONTROL" "$DYNDBG_CONTROL"
	exit 0
fi
exit 1
STUB
chmod +x "$STUBDIR/dyndbg-writer" "$STUBDIR/mount"

WRITER_LOG="$TMPROOT/writer.log"
export WRITER_LOG
run_afe_debug() {
	# run_afe_debug <args...>; stdout to $OUT, stderr to $ERR
	: > "$WRITER_LOG"
	DYNDBG_CONTROL="$CONTROL" DYNDBG_WRITER="$STUBDIR/dyndbg-writer" \
		MOUNT="$STUBDIR/mount" DEBUGFS_DIR="$TMPROOT/debugfs" \
		sh "$SCRIPT" "$@" > "$TMPROOT/out" 2> "$TMPROOT/err"
}

flags_of() {
	awk -v name="$1" '{ f = $2; sub(/^\[[^]]*\]/, "", f); if (f == name) print $3 }' "$CONTROL" | sort -u | tr '\n' ' '
}

# ---- usage ----
write_listing
if run_afe_debug; then bad "no argument must fail"; else ok; fi
if run_afe_debug bogus; then bad "unknown verb must fail"; else ok; fi
if run_afe_debug on off; then bad "two verbs must fail"; else ok; fi
eq "usage failures write nothing" "" "$(cat "$WRITER_LOG")"

# ---- status on an all-off listing ----
write_listing
if run_afe_debug status; then ok; else bad "status must succeed with a readable control file"; fi
eq "status lists every site with its flags, absent ones flagged" \
	"$(for n in $SITES; do echo "$n: =_ "; done)" "$(cat "$TMPROOT/out")"
eq "status writes nothing" "" "$(cat "$WRITER_LOG")"

# ---- on: every site +p, verified; siblings untouched ----
write_listing
if run_afe_debug on; then ok; else bad "on must succeed: $(cat "$TMPROOT/err")"; fi
for n in $SITES; do
	eq "on sets $n to =p on every line" "=p " "$(flags_of "$n")"
done
eq "on leaves the unrelated q6adm site off" "=_ " "$(flags_of adm_callback)"
eq "on writes exactly one +p query per site, in list order" \
	"$(for n in $SITES; do echo "func $n +p"; done)" "$(cat "$WRITER_LOG")"
case "$(cat "$TMPROOT/err")" in
*"enabled 11 q6afe/tas2560 debug sites"*) ok ;;
*) bad "on must report the enabled site count, got: $(cat "$TMPROOT/err")" ;;
esac
eq "afe_callback_debug_print's two lines are both =p" "2" "$(awk '$2 == "[q6afe]afe_callback_debug_print" && $3 == "=p"' "$CONTROL" | wc -l | tr -d ' ')"

# ---- whole-name matching: with afe_callback_debug_print left unchanged by
# the (stub) kernel, the verification of afe_callback must NOT be confused
# by the longer sibling's stale "=_" lines -- only the sibling is reported.
write_listing
if STUB_IGNORE_FUNC=afe_callback_debug_print run_afe_debug on; then
	bad "on must fail when afe_callback_debug_print stays unchanged"
else
	ok
fi
case "$(cat "$TMPROOT/err")" in
*"function afe_callback_debug_print has flags '=_' after '+p'"*) ok ;;
*) bad "on must report afe_callback_debug_print, got: $(cat "$TMPROOT/err")" ;;
esac
case "$(cat "$TMPROOT/err")" in
*"function afe_callback has flags"*) bad "afe_callback must verify on its own lines only (prefix match leaked)" ;;
*) ok ;;
esac
eq "afe_callback itself was set" "=p " "$(flags_of afe_callback)"

# ---- off after on: everything back to =_ ----
if run_afe_debug off; then ok; else bad "off must succeed: $(cat "$TMPROOT/err")"; fi
for n in $SITES; do
	eq "off resets $n to =_" "=_ " "$(flags_of "$n")"
done
eq "off writes exactly one -p query per site" \
	"$(for n in $SITES; do echo "func $n -p"; done)" "$(cat "$WRITER_LOG")"

# ---- a site the kernel silently ignores (renamed function): the write
# "succeeds" but the listing shows it unchanged -> on must fail and say so,
# while still applying the other sites. ----
write_listing
if STUB_IGNORE_FUNC=afe_send_port_topology_id run_afe_debug on; then
	bad "on must fail when a site's flags do not change after the write"
else
	ok
fi
case "$(cat "$TMPROOT/err")" in
*"afe_send_port_topology_id has flags '=_' after '+p', expected =p"*) ok ;;
*) bad "on must name the site whose flags did not change, got: $(cat "$TMPROOT/err")" ;;
esac
eq "the other sites were still enabled" "=p " "$(flags_of afe_callback_debug_print)"
eq "the ignored site stayed off" "=_ " "$(flags_of afe_send_port_topology_id)"
# and off must still run over all sites afterwards
if run_afe_debug off; then ok; else bad "off after a partial on must still succeed"; fi

# ---- a site missing from the listing entirely ----
write_listing
grep -v 'tas2560_algo_afe_get_param' "$CONTROL" > "$CONTROL.new" && mv "$CONTROL.new" "$CONTROL"
if run_afe_debug on; then bad "on must fail when a site is absent from the listing"; else ok; fi
case "$(cat "$TMPROOT/err")" in
*"no dynamic-debug site for function tas2560_algo_afe_get_param"*) ok ;;
*) bad "on must name the absent site, got: $(cat "$TMPROOT/err")" ;;
esac
if run_afe_debug status; then ok; else bad "status must still succeed with an absent site"; fi
case "$(cat "$TMPROOT/out")" in
*"tas2560_algo_afe_get_param: absent"*) ok ;;
*) bad "status must print 'absent' for a missing site" ;;
esac

# ---- the write itself failing ----
write_listing
if STUB_WRITER_FAIL=1 run_afe_debug on; then bad "on must fail when the control write fails"; else ok; fi
case "$(cat "$TMPROOT/err")" in
*"write 'func afe_callback_debug_print +p' to $CONTROL failed"*) ok ;;
*) bad "on must report the failed write, got: $(cat "$TMPROOT/err")" ;;
esac

# ---- debugfs not mounted: control file absent -> mount is attempted;
# with a successful mount the file appears and on proceeds; with a failed
# mount on/off/status exit 1 without any query. ----
write_listing
PREPARED_CONTROL="$CONTROL"
export PREPARED_CONTROL
MOUNTED_CONTROL="$TMPROOT/debugfs/dynamic_debug/control"
rm -rf "$TMPROOT/debugfs"
: > "$WRITER_LOG"
if STUB_MOUNT_CREATES=1 DYNDBG_CONTROL="$MOUNTED_CONTROL" DYNDBG_WRITER="$STUBDIR/dyndbg-writer" \
	MOUNT="$STUBDIR/mount" DEBUGFS_DIR="$TMPROOT/debugfs" \
	sh "$SCRIPT" on > "$TMPROOT/out" 2> "$TMPROOT/err"; then ok; else bad "on must succeed after mounting debugfs: $(cat "$TMPROOT/err")"; fi
eq "mount is invoked as debugfs on DEBUGFS_DIR before the queries" \
	"mount -t debugfs debugfs $TMPROOT/debugfs" "$(head -n 1 "$WRITER_LOG")"
eq "queries follow the mount" "func afe_callback_debug_print +p" "$(sed -n 2p "$WRITER_LOG")"
eq "the mounted control file ends up with the site enabled" "=p " \
	"$(awk -v name=afe_callback_debug_print '{ f = $2; sub(/^\[[^]]*\]/, "", f); if (f == name) print $3 }' "$MOUNTED_CONTROL" | sort -u | tr '\n' ' ')"

rm -rf "$TMPROOT/debugfs"
: > "$WRITER_LOG"
if DYNDBG_CONTROL="$MOUNTED_CONTROL" DYNDBG_WRITER="$STUBDIR/dyndbg-writer" \
	MOUNT="$STUBDIR/mount" DEBUGFS_DIR="$TMPROOT/debugfs" \
	sh "$SCRIPT" on > "$TMPROOT/out" 2> "$TMPROOT/err"; then bad "on must fail when debugfs cannot be mounted"; else ok; fi
eq "a failed mount attempts no query" "mount -t debugfs debugfs $TMPROOT/debugfs" "$(cat "$WRITER_LOG")"
case "$(cat "$TMPROOT/err")" in
*"control file $MOUNTED_CONTROL is unavailable"*) ok ;;
*) bad "on must explain the missing control file, got: $(cat "$TMPROOT/err")" ;;
esac
if DYNDBG_CONTROL="$MOUNTED_CONTROL" MOUNT="$STUBDIR/mount" DEBUGFS_DIR="$TMPROOT/debugfs" \
	sh "$SCRIPT" status > "$TMPROOT/out" 2> "$TMPROOT/err"; then bad "status must fail without a control file"; else ok; fi

# ---- production default: writes go to the control file itself ----
default=$(sed -n 's/^DYNDBG_CONTROL=\${DYNDBG_CONTROL:-\(.*\)}$/\1/p' "$SCRIPT")
eq "default control path is DEBUGFS_DIR/dynamic_debug/control" '$DEBUGFS_DIR/dynamic_debug/control' "$default"
eq "default debugfs dir is /sys/kernel/debug" "/sys/kernel/debug" "$(sed -n 's/^DEBUGFS_DIR=\${DEBUGFS_DIR:-\(.*\)}$/\1/p' "$SCRIPT")"
if grep -q 'echo "\$1" > "\$DYNDBG_CONTROL"' "$SCRIPT"; then ok; else bad "the production writer must write the query to the control file"; fi

# A failing run must SAY so rather than print "PASS:" regardless and leave
# the exit status as the only signal -- the last line of a suite's output is
# what a reader (and `make test`'s log) actually sees. Exit behaviour is
# unchanged: 0 only when nothing failed.
if [ "$fail" -eq 0 ]; then
	echo "PASS: $pass/$((pass + fail)) checks passed"
else
	echo "FAILED: $fail of $((pass + fail)) checks failed ($pass passed)" >&2
	exit 1
fi
