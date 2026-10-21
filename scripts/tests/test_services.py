"""OpenRC services (docs/features/services.md): the scripts, their wiring and
the logic that moved out of inittab into them.

The packaged parts (Alpine's init.d scripts, rc.conf, supervise-daemon.sh,
start-stop-daemon.sh) come from out/rootfs when it was built with OpenRC,
else from the apk cache under toolchain/apk/cache; the tests that need them
skip when neither has them. Hooks run in a harness that sources the files in
openrc-run's order (rc.conf, rc.conf.d, conf.d, the supervisor helpers, the
script), with the OpenRC helpers (ebegin, service_get_value, ...) as shell
functions and supervise-daemon and start-stop-daemon as recording stubs, so
a test sees the exact arguments a start would pass. Not covered here: what
OpenRC itself does with them (the live check).
"""
import gzip
import io
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tarfile
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[2]
OVERLAY = REPO / "rootfs-overlay"
CACHE = REPO / "toolchain/apk/cache"

# The default runlevel: every service the image starts.
DEFAULT = {"syslog", "udhcpd", "dbus", "bluetooth", "root-session-bus", "modem-up", "networkmanager",
           "chronyd", "chef-timekeeper", "bt-up", "blesensd", "buttond", "chef-sshd", "gps-manager",
           "powerd", "sensors-up", "chef-settings"}
# Alpine's scripts we run, adjusted by conf.d only, and the package each comes from.
PACKAGED = {"syslog": "busybox-openrc", "udhcpd": "busybox-extras-openrc", "dbus": "dbus-openrc",
            "bluetooth": "bluez-openrc", "chronyd": "chrony-openrc", "networkmanager": "networkmanager-openrc"}
# What each packaged depend() names outside our runlevel (Alpine 3.24): the
# Alpine boot services, and the firewall (iptables-openrc, not installed);
# conf.d removes every need, want and after on them. A test checks the list
# against the scripts themselves.
ALPINE_DEPS = {
    "syslog": {"need": {"clock", "hostname", "localmount"}},
    "udhcpd": {"need": {"net"}, "after": {"firewall"}},
    "dbus": {"need": {"localmount"}, "after": {"bootmisc"}},
    "bluetooth": {"need": {"localmount", "hostname"}, "after": {"coldplug", "udev-postmount"}},
    "chronyd": {"need": {"net"}, "after": {"firewall"}},
    "networkmanager": {},
}
# The inittab lines, all of them: everything else is an OpenRC service.
INITTAB = [
    "::respawn:/bin/sh -c 'export HOME=/root XDG_RUNTIME_DIR=/run/user/0 "
    "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus; exec /usr/sbin/telnetd -F -b 172.16.42.1 -l /bin/sh'",
    "::once:/bin/sh -c '/sbin/openrc sysinit; exec /sbin/openrc default'",
    "::once:/bin/sh -c 'sleep 30; exec /usr/bin/abslot mark-successful >/dev/null'",
    "::ctrlaltdel:/sbin/reboot",
    "::shutdown:/usr/bin/timeout -k 1 5 /usr/bin/chef-state shutdown",
    "::shutdown:/usr/bin/timeout -k 1 3 /usr/bin/chef-state log-save",
    "::shutdown:/usr/bin/timeout -k 1 20 /sbin/openrc shutdown",
    "::shutdown:/usr/bin/timeout -k 1 10 /usr/bin/chef-stop-supervisors",
    "::shutdown:/usr/bin/chef-storage shutdown",
    "::shutdown:/bin/umount -a -r",
    "::restart:/bin/sh -c '/usr/bin/timeout -k 1 5 /usr/bin/chef-state shutdown; /usr/bin/chef-storage shutdown; exec /sbin/init'",
]

_apk_cache = {}


def apk_files(package):
    """{path: bytes} of a package's etc/ and usr/libexec/rc/sh/ files in the apk cache."""
    if package not in _apk_cache:
        files = {}
        for apk in sorted(CACHE.glob(f"{package}-[0-9]*.apk")) if CACHE.is_dir() else []:
            with tarfile.open(fileobj=io.BytesIO(gzip.decompress(apk.read_bytes())), ignore_zeros=True) as t:
                for m in t.getmembers():
                    if m.isfile() and m.name.startswith(("etc/", "usr/libexec/rc/sh/")):
                        files[m.name] = t.extractfile(m).read()
        _apk_cache[package] = files
    return _apk_cache[package]


def apk_file(package, path):
    """A file from a package in the apk cache, or None."""
    data = apk_files(package).get(path)
    return None if data is None else data.decode()


def packaged(package, path):
    """A packaged file from out/rootfs (built with OpenRC) or the apk cache."""
    built = REPO / "out/rootfs" / path
    if built.is_file():
        return built.read_text()
    text = apk_file(package, path)
    if text is None:
        raise unittest.SkipTest(f"no {path} ({package}) in out/rootfs or {CACHE}")
    return text


def own_scripts():
    return sorted((OVERLAY / "etc/init.d").iterdir())


# The OpenRC helpers a hook may call, as shell functions; supervise-daemon
# and start-stop-daemon are stubs on PATH that record their arguments.
HELPERS = r'''
yesno() { case "$1" in [Yy][Ee][Ss]|[Tt][Rr][Uu][Ee]|[Oo][Nn]|1) return 0;; esac; return 1; }
ebegin() { :; }
eend() { [ "${1:-0}" = 0 ] || echo "eend $*" >> "$OUT"; return "${1:-0}"; }
einfo() { :; }
ewarn() { echo "ewarn $*" >> "$OUT"; }
eerror() { echo "eerror $*" >> "$OUT"; }
ewaitfile() { echo "ewaitfile $*" >> "$OUT"; return "${EWAITFILE_RC:-0}"; }
service_set_value() { echo "$2" > "$VALS/$1"; }
service_get_value() { cat "$VALS/$1" 2>/dev/null; }
checkpath() { echo "checkpath $*" >> "$OUT"; }
# openrc-run.sh's templates, which a script may override
default_start() { if [ "$supervisor" = supervise-daemon ]; then supervise_start; else ssd_start; fi; }
default_stop() { if [ "$supervisor" = supervise-daemon ]; then supervise_stop; else ssd_stop; fi; }
start() { default_start; }
stop() { default_stop; }
'''
# the supervisors, and the tools hooks call (never the host's)
STUBS = ("supervise-daemon", "start-stop-daemon", "iptables-legacy", "dbus-uuidgen")
STUB = '#!/bin/sh\nprintf "%s" "${0##*/}" >> "$OUT"; for a; do printf " [%s]" "$a" >> "$OUT"; done; echo >> "$OUT"\n'


class Harness:
    """Runs hooks of a service the way openrc-run would, against stubs."""

    def __init__(self, test, shell=("sh",)):
        self.test, self.shell = test, list(shell)
        self.tmp = Path(tempfile.mkdtemp())
        test.addCleanup(shutil.rmtree, self.tmp)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        for name in STUBS:
            (self.bin / name).write_text(STUB)
            (self.bin / name).chmod(0o755)
        lib = self.tmp / "lib"
        lib.mkdir()
        (lib / "rc.conf").write_text(packaged("openrc", "etc/rc.conf"))
        for sh in ("supervise-daemon.sh", "start-stop-daemon.sh"):
            (lib / sh).write_text(packaged("openrc", "usr/libexec/rc/sh/" + sh))

    def script(self, service):
        if service in PACKAGED:
            return packaged(PACKAGED[service], f"etc/init.d/{service}")
        return (OVERLAY / "etc/init.d" / service).read_text()

    def run(self, service, body, replace=(), env=None, **runlevel):
        """Source everything for SERVICE, then run BODY (shell)."""
        d = self.tmp / service
        (d / "vals").mkdir(parents=True, exist_ok=True)
        files = {"conf": OVERLAY / "etc/conf.d" / service, "chef": OVERLAY / "etc/rc.conf.d/chef.conf"}
        texts = {k: (p.read_text() if p.exists() else "") for k, p in files.items()}
        texts["script"] = self.script(service)
        for k in texts:
            for old, new in replace:
                texts[k] = texts[k].replace(old, new)
            (d / k).write_text(texts[k])
        out = self.tmp / "out"
        out.write_text("")
        # BusyBox ash runs its own applets (start-stop-daemon) before PATH:
        # bind the stubs as functions there (dash rejects such names).
        bind = "".join(f'{n}() {{ "$STUBS/{n}" "$@"; }}\n' for n in STUBS) if "busybox" in self.shell[0] else ""
        code = (HELPERS + bind + f'RC_SVCNAME={service}\n. "$LIB/rc.conf"\n. "$D/chef"\n. "$D/conf"\n'
                f'. "$LIB/supervise-daemon.sh"\n. "$LIB/start-stop-daemon.sh"\n. "$D/script"\n' + body)
        r = subprocess.run([*self.shell, "-c", code], text=True, capture_output=True, timeout=20,
                           env={**os.environ, "PATH": f"{self.bin}:{os.environ['PATH']}", "OUT": str(out),
                                "VALS": str(d / "vals"), "STUBS": str(self.bin), "LIB": str(self.tmp / "lib"), "D": str(d),
                                **runlevel, **(env or {})})
        return r, out.read_text()

    def start(self, service, **kw):
        """start_pre (if any) and start: the supervisor's command line."""
        r, out = self.run(service, '[ "$(command -v start_pre)" != start_pre ] || start_pre || exit $?\nstart\n', **kw)
        self.test.assertEqual(r.returncode, 0, (r.stderr, out))
        lines = [l for l in out.splitlines() if l.startswith(("supervise-daemon ", "start-stop-daemon "))]
        self.test.assertEqual(len(lines), 1, out)
        return re.findall(r"\[(.*?)\]", lines[0]), lines[0].split()[0]


class Services(unittest.TestCase):
    shell = ("sh",)

    def test_every_script_parses(self):
        for f in [*own_scripts(), *(OVERLAY / "etc/conf.d").iterdir(), OVERLAY / "etc/rc.conf.d/chef.conf"]:
            r = subprocess.run([*self.shell, "-n", str(f)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, (f, r.stderr))
        for f in own_scripts():
            self.assertEqual(f.read_text().splitlines()[0], "#!/sbin/openrc-run", f)
            self.assertTrue(os.access(f, os.X_OK), f)
        for svc, pkg in PACKAGED.items():
            with tempfile.NamedTemporaryFile("w", suffix=svc) as f:
                f.write(packaged(pkg, f"etc/init.d/{svc}"))
                f.flush()
                r = subprocess.run([*self.shell, "-n", f.name], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, (svc, r.stderr))

    def test_runlevel_links(self):
        level = OVERLAY / "etc/runlevels/default"
        self.assertEqual({p.name for p in level.iterdir()}, DEFAULT)
        for link in level.iterdir():
            self.assertTrue(link.is_symlink(), link)
            self.assertEqual(os.readlink(link), f"/etc/init.d/{link.name}")
            if link.name in PACKAGED:
                packaged(PACKAGED[link.name], f"etc/init.d/{link.name}")
            else:
                self.assertTrue((OVERLAY / "etc/init.d" / link.name).is_file(), link)
        # every script we ship is in the runlevel; conf.d only for scripts that exist
        self.assertEqual({p.name for p in own_scripts()}, DEFAULT - set(PACKAGED))
        self.assertEqual({p.name for p in (OVERLAY / "etc/conf.d").iterdir()}, set(PACKAGED))
        for other in ("sysinit", "boot", "shutdown", "nonetwork"):
            self.assertFalse((OVERLAY / "etc/runlevels" / other).exists(), other)

    def test_inittab_has_only_the_named_lines(self):
        lines = [l for l in (OVERLAY / "etc/inittab").read_text().splitlines() if l and not l.startswith("#")]
        self.assertEqual(lines, INITTAB)

    def depends(self, service):
        """need/use/want/after/before as gendepends.sh sees them, rc_* included."""
        h = Harness(self, self.shell)
        body = ('set -f\n'
                'for t in need use want after before provide; do eval "$t() { echo $t \\"\\$*\\" >> \\"\\$OUT\\"; }"; done\n'
                'depend\nfor t in need use want after before provide; do eval d=\\$rc_$t; [ -z "$d" ] || $t $d; done\n')
        r, out = h.run(service, body)
        self.assertEqual(r.returncode, 0, (service, r.stderr))
        deps = {}
        for line in out.splitlines():
            kind, *names = line.split()
            deps.setdefault(kind, []).extend(names)
        # librc: "!name" deletes that dependency
        return {k: {n for n in v if not n.startswith("!")} - {n[1:] for n in v if n.startswith("!")}
                for k, v in deps.items()}

    def test_nothing_outside_the_runlevel_starts(self):
        # Packaged scripts: the list above is what they name, and conf.d removes it.
        for svc in PACKAGED:
            text = packaged(PACKAGED[svc], f"etc/init.d/{svc}")
            body = text.split("depend()", 1)[1].split("}", 1)[0]
            named = {}
            for line in body.splitlines():
                words = line.split()
                if words and words[0] in ("need", "want", "after"):
                    named.setdefault(words[0], set()).update(w for w in words[1:] if w not in DEFAULT)
            self.assertEqual({k: v for k, v in named.items() if v}, ALPINE_DEPS[svc], svc)
        # Every service: what it needs or wants is in the runlevel (a need
        # starts the needed service even outside it), no Alpine boot service
        # is ordered against it, and syslog (logger) comes first.
        for svc in sorted(DEFAULT):
            with self.subTest(svc=svc):
                deps = self.depends(svc)
                for kind in ("need", "want"):
                    self.assertLessEqual(deps.get(kind, set()), DEFAULT, (svc, kind))
                for kind, names in ALPINE_DEPS.get(svc, {}).items():
                    self.assertFalse(deps.get(kind, set()) & names, (svc, kind))
                if svc != "syslog":
                    self.assertTrue("logger" in deps.get("use", set()) or "*" in deps.get("after", set()), svc)
        self.assertEqual(self.depends("syslog").get("provide"), {"logger"})
        self.assertEqual(self.depends("networkmanager").get("need"), {"dbus", "modem-up"})
        self.assertEqual(self.depends("networkmanager").get("after"), {"*"})
        self.assertEqual(self.depends("bluetooth").get("need"), {"dbus"})
        self.assertIn("bluetooth", self.depends("blesensd").get("after"))
        self.assertIn("modem-up", self.depends("sensors-up").get("after"))
        self.assertIn("modem-up", self.depends("gps-manager").get("after"))
        # the screen first, then the modem, then the rest
        self.assertLessEqual({"dbus", "udhcpd", "modem-up", "chronyd", "bt-up"}, self.depends("chef-settings")["before"])
        self.assertLessEqual({"dbus", "udhcpd", "chronyd", "bt-up"}, self.depends("modem-up")["before"])

    def test_supervision_settings_reach_every_daemon(self):
        h = Harness(self, self.shell)
        for svc in sorted(DEFAULT - {"modem-up"}):
            with self.subTest(svc=svc):
                args, tool = h.start(svc, replace=[("/usr/bin/chef-state ours", "false"),
                                                   ("$(/usr/bin/wifi-nm)", "--state-file=/x"),
                                                   ("/run/chef/log/sensors-up.log", str(h.tmp / "up.log")),
                                                   ("/run/user/0", str(h.tmp / "user0"))])
                self.assertEqual(tool, "supervise-daemon")
                self.assertEqual(args[:2], [svc, "--start"])
                opt = dict(zip(args, args[1:]))
                self.assertEqual(opt["--respawn-delay"], "10s" if svc == "sensors-up" else "1s")
                self.assertEqual(opt["--respawn-max"], "0")
                self.assertEqual(opt["--retry"], "TERM/5/KILL/2")
                # the package's -g wheel -k 027 never reaches it
                self.assertNotIn("-g", args)
        # modem-up: started once by start-stop-daemon, backgrounded, logged
        args, tool = h.start("modem-up")
        self.assertEqual(tool, "start-stop-daemon")
        opt = dict(zip(args, args[1:]))
        self.assertEqual(opt["--exec"], "/usr/bin/modem-up")
        self.assertEqual(opt["--stdout"], "/run/chef/log/modem-up.log")
        self.assertEqual(opt["--stderr"], "/run/chef/log/modem-up.log")
        self.assertIn("--background", args)
        self.assertIn("--make-pidfile", args)

    def test_packaged_daemons_run_in_the_foreground_as_before(self):
        h = Harness(self, self.shell)
        cmd = {}
        for svc in PACKAGED:
            args, _ = h.start(svc, replace=[("/usr/bin/chef-state ours", "false"),
                                            ("$(/usr/bin/wifi-nm)", "--state-file=/x")])
            opt = dict(zip(args, args[1:]))
            cmd[svc] = (opt.get("--pidfile"), args[args.index("--") - 1], args[args.index("--") + 1:])
        self.assertEqual(cmd["syslog"], ("/run/supervise-syslog.pid", "/sbin/syslogd", ["-t", "-C1024", "-n"]))
        # -f before the config path, whatever getopt does with the package's trailing -f
        self.assertEqual(cmd["udhcpd"], ("/run/supervise-udhcpd.pid", "/usr/sbin/udhcpd",
                                         ["-f", "-S", "/run/chef/udhcpd.conf", "-f"]))
        self.assertEqual(cmd["dbus"][1:], ("/usr/bin/dbus-daemon", ["--system", "--nofork", "--nopidfile", "--syslog-only"]))
        self.assertEqual(cmd["bluetooth"][1:], ("/usr/lib/bluetooth/bluetoothd", ["-n"]))
        self.assertEqual(cmd["networkmanager"][1:], ("/usr/sbin/NetworkManager", ["--state-file=/x", "-n"]))
        self.assertEqual(cmd["chronyd"][1], "/usr/sbin/chronyd")
        self.assertEqual(cmd["chronyd"][2][:6], ["-f", "/etc/chrony/chrony.conf", "-F", "1", "-u", "chrony"])
        self.assertEqual(cmd["chronyd"][2][-1], "-n")
        # the daemons' own pid files are never the supervisor's
        for svc, own in (("syslog", "/run/syslogd.pid"), ("udhcpd", "/run/udhcpd.pid")):
            self.assertNotEqual(cmd[svc][0], own)
            r, out = h.run(svc, 'stop_pre; supervise_stop\n')
            self.assertIn(f"--stop] [--pidfile] [/run/supervise-{svc}.pid]", out)
        self.assertNotIn(cmd["chronyd"][0], ("/run/chrony/chronyd.pid",))
        conf = (OVERLAY / "etc/chrony/chrony.conf").read_text()
        self.assertIn("pidfile /run/chrony/chronyd.pid", conf)

    def test_preconditions_fail_the_start(self):
        h = Harness(self, self.shell)
        # udhcpd and dbus: markers /init writes once
        self.assertEqual(h.run("udhcpd", 'echo "$required_files" >> "$OUT"')[1].strip(), "/run/chef/udhcpd.conf")
        self.assertEqual(h.run("dbus", 'echo "$required_files" >> "$OUT"')[1].strip(), "/run/chef/dbus-helper-ready")
        # dbus: the bus socket within 10 s, else the start fails and its supervisor goes
        r, out = h.run("dbus", "start_post")
        self.assertEqual((r.returncode, out), (0, "ewaitfile 10 /run/dbus/system_bus_socket\n"))
        r, out = h.run("dbus", "start_post", env={"EWAITFILE_RC": "1"})
        self.assertEqual(r.returncode, 1)
        self.assertIn("eerror dbus: no /run/dbus/system_bus_socket after 10 s", out)
        self.assertIn("supervise-daemon [dbus] [--stop]", out)
        # the machine id /init ensured, not the read-only root's
        self.assertIn("machine_id=/var/lib/dbus/machine-id", (OVERLAY / "etc/conf.d/dbus").read_text())
        # NetworkManager: wifi-nm's output becomes its arguments; a failure fails the start
        for rc, want in ((0, 0), (1, 1)):
            stub = h.tmp / "wifi-nm"
            stub.write_text(f'#!/bin/sh\necho "--state-file=/s"\nexit {rc}\n')
            stub.chmod(0o755)
            r, out = h.run("networkmanager", 'start_pre || exit $?; echo "args $command_args" >> "$OUT"',
                           replace=[("/usr/bin/wifi-nm", str(stub))])
            self.assertEqual(r.returncode, want)
            if not rc:
                self.assertIn("args --state-file=/s", out)

    def test_bnep_guard(self):
        h = Harness(self, self.shell)
        ipt = h.bin / "iptables-legacy"
        logger = h.tmp / "logger"
        logger.write_text('#!/bin/sh\necho "logger $*" >> "$OUT"\n')
        logger.chmod(0o755)
        lib = [("/usr/lib/chef/log.sh", str(OVERLAY / "usr/lib/chef/log.sh"))]
        env = {"CHEF_LOG_KMSG": str(h.tmp / "kmsg"), "CHEF_LOG_SYSLOGD_PID": str(h.tmp / "none"),
               "CHEF_LOG_LOGGER": str(logger)}
        (h.tmp / "kmsg").write_text("")
        for check, insert, rc in ((0, 1, 0), (1, 0, 0), (1, 1, 1)):
            ipt.write_text(f'#!/bin/sh\necho "iptables $*" >> "$OUT"\ncase "$*" in *-C*) exit {check};; esac\nexit {insert}\n')
            ipt.chmod(0o755)
            r, out = h.run("bluetooth", "start_pre", replace=lib, env=env)
            self.assertEqual(r.returncode, rc, out)
            self.assertIn("iptables -w 5 -C INPUT -i bnep+ -d 172.16.42.0/24 -j DROP", out)
            self.assertEqual("-I INPUT 1 -i bnep+" in out, bool(check))
        self.assertIn("bnep-guard: BNEP ingress guard failed, bluetoothd not started", (h.tmp / "kmsg").read_text())

    def test_powerd_log_on_data_only_when_ours(self):
        h = Harness(self, self.shell)
        for ours, want in (("true", "-l /data/v1/power -L 4194304"), ("false", "")):
            r, out = h.run("powerd", 'start_pre; echo "args=$command_args" >> "$OUT"',
                           replace=[("/usr/bin/chef-state ours", ours)])
            self.assertEqual(out.strip(), "args=" + want)
        # chef-state's fallback flush uses the same directory and size
        state = (OVERLAY / "usr/bin/chef-state").read_text()
        self.assertIn("\nPOWER_LOG_MAX=4194304\t", state)
        self.assertIn('-l "$DATA/v1/power" -L "$POWER_LOG_MAX" flush', state)

    def test_sensors_up_log_rotation_and_environment(self):
        h = Harness(self, self.shell)
        log = h.tmp / "up.log"
        rep = [("/run/chef/log/sensors-up.log", str(log))]
        body = 'start_pre; echo "args=[${SENSORD_ARGS-unset}] err=$SENSORD_STDERR" >> "$OUT"'
        log.write_text("x" * 1000)
        r, out = h.run("sensors-up", body, replace=rep)
        self.assertEqual((r.returncode, out), (0, "args=[] err=/dev/null\n"))
        self.assertFalse(Path(str(log) + ".1").exists())
        log.write_text("y" * 300000)
        r, out = h.run("sensors-up", body, replace=rep)
        self.assertEqual(r.returncode, 0)
        self.assertFalse(log.exists())
        self.assertEqual(Path(str(log) + ".1").read_text(), "y" * 300000)
        log.write_text("z")
        self.assertEqual(h.run("sensors-up", body, replace=rep)[0].returncode, 0)
        self.assertEqual(Path(str(log) + ".1").read_text(), "y" * 300000)

    def test_root_session_bus_environment(self):
        h = Harness(self, self.shell)
        d = h.tmp / "user0"
        r, out = h.run("root-session-bus", 'start_pre; echo "$HOME $XDG_RUNTIME_DIR" >> "$OUT"; start',
                       replace=[("/run/user/0", str(d))])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(oct(d.stat().st_mode & 0o777), "0o700")
        self.assertIn(f"/root {d}", out)
        self.assertIn(f"[--session] [--nofork] [--nopidfile] [--address=unix:path={d}/bus]", out)

    def test_modem_up_stops_only_at_shutdown(self):
        h = Harness(self, self.shell)
        for level, rc in (("default", 1), ("", 1), ("shutdown", 0)):
            r, out = h.run("modem-up", "stop", RC_RUNLEVEL=level)
            self.assertEqual(r.returncode, rc, (level, out))
            self.assertEqual("start-stop-daemon [--stop]" in out, rc == 0, out)
            if rc:
                self.assertIn("eerror modem-up stops only at shutdown", out)
        self.assertIn("[--retry] [TERM/10/KILL/2]", out)
        self.assertIn("[--pidfile] [/run/chef/modem-up.pid]", out)

    def test_stop_takes_the_whole_process_group(self):
        h = Harness(self, self.shell)
        for svc in ("chef-timekeeper", "bt-up", "chef-sshd"):
            with self.subTest(svc=svc):
                # a leader that is already gone and its children, alone in their group
                leader = subprocess.Popen(["sh", "-c", "sleep 30 & sleep 30 & sleep 0.2"], start_new_session=True)
                pgid = leader.pid
                leader.wait()
                time.sleep(0.1)
                self.assertTrue(self.members(pgid))
                (h.tmp / svc / "vals").mkdir(parents=True, exist_ok=True)
                (h.tmp / svc / "vals/child_pid").write_text(f"{pgid}\n")
                r, out = h.run(svc, "stop_pre; stop_post")
                self.assertEqual(r.returncode, 0, out)
                for _ in range(50):
                    if not self.members(pgid):
                        break
                    time.sleep(0.05)
                self.assertEqual(self.members(pgid), [])
                # nothing to do without a child pid
                (h.tmp / svc / "vals/child_pid").write_text("")
                self.assertEqual(h.run(svc, "stop_pre; stop_post")[0].returncode, 0)

    @staticmethod
    def members(pgid):
        found = []
        for d in Path("/proc").iterdir():
            try:
                fields = (d / "stat").read_text().rsplit(")", 1)[1].split()
            except (OSError, IndexError):
                continue
            if fields[0] not in "ZX" and int(fields[2]) == pgid:
                found.append(int(d.name))
        return found

    def stop_supervisors(self, procs, wait="5"):
        """chef-stop-supervisors on a /proc that holds only PROCS (links)."""
        with tempfile.TemporaryDirectory() as tmp:
            proc, kmsg = Path(tmp) / "proc", Path(tmp) / "kmsg"
            proc.mkdir()
            kmsg.write_text("")
            for pr in procs:
                (proc / str(pr.pid)).symlink_to(f"/proc/{pr.pid}")
            r = subprocess.run([*self.shell, str(OVERLAY / "usr/bin/chef-stop-supervisors")], capture_output=True,
                               text=True, timeout=20,
                               env={**os.environ, "CHEF_STOP_PROC": str(proc), "CHEF_STOP_KMSG": str(kmsg),
                                    "CHEF_STOP_WAIT": wait, "CHEF_LIB_DIR": str(OVERLAY / "usr/lib/chef")})
            return r, kmsg.read_text()

    def fake_supervisor(self, service, stubborn=False):
        """A process whose comm is supervise-daemon and whose command line names SERVICE."""
        d = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, d)
        exe = d / "supervise-daemon"
        exe.write_text("#!/bin/sh\n" + ("trap '' TERM\n" if stubborn else "trap 'exit 0' TERM\n") +
                       "while :; do sleep 0.05; done\n")
        exe.chmod(0o755)
        pr = subprocess.Popen([str(exe), service, "--start", "--pidfile", "/x"])
        self.addCleanup(lambda: (pr.kill(), pr.wait()))
        return pr

    def test_stop_supervisors(self):
        # nothing left: silent
        other = subprocess.Popen(["sleep", "30"])
        self.addCleanup(lambda: (other.kill(), other.wait()))
        r, kmsg = self.stop_supervisors([other])
        self.assertEqual((r.returncode, kmsg), (0, ""))
        # supervisors left: TERM, wait, log
        a, b = self.fake_supervisor("modem-up"), self.fake_supervisor("syslog")
        time.sleep(0.2)
        r, kmsg = self.stop_supervisors([a, b, other])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((a.wait(timeout=5), b.wait(timeout=5)), (0, 0))
        self.assertIn("chef-stop-supervisors: openrc shutdown left ", kmsg)
        self.assertRegex(kmsg, r"left (modem-up syslog|syslog modem-up) running; stopping their supervisors")
        self.assertIn("chef-stop-supervisors: every supervisor stopped", kmsg)
        self.assertIsNone(other.poll())
        # one ignores TERM: KILL after the wait, exit 1
        c, d = self.fake_supervisor("chef-sshd", stubborn=True), self.fake_supervisor("udhcpd")
        time.sleep(0.2)
        start = time.monotonic()
        r, kmsg = self.stop_supervisors([c, d])
        self.assertLess(time.monotonic() - start, 5)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(c.wait(timeout=5), -signal.SIGKILL)
        self.assertEqual(d.wait(timeout=5), 0)
        self.assertIn("supervisors of chef-sshd still running after 0 s; KILL", kmsg)
        text = (OVERLAY / "usr/bin/chef-stop-supervisors").read_text()
        self.assertIn("\nWAIT=${CHEF_STOP_WAIT:-80}\t", text)

    def test_init_prepares_chrony_directories(self):
        init = (OVERLAY / "init").read_text()
        line = "mkdir -p /run/chrony /var/lib/chrony && chown chrony:chrony /run/chrony /var/lib/chrony &&"
        self.assertIn(line, init)
        self.assertLess(init.index(line), init.index("/usr/bin/chef-storage boot"))

    def test_image_keeps_only_our_scripts(self):
        # mkinitramfs.sh's prune block, on a root holding every packaged
        # init.d script and conf.d default (each -openrc package we install)
        # with the overlay on top, as the build stages it.
        block = (REPO / "scripts/mkinitramfs.sh").read_text().split("--- OpenRC prune ---\n", 1)[1]
        block = block.split("# --- end OpenRC prune ---", 1)[0]
        pkgs = ["openrc", *sorted(set(PACKAGED.values()))]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for d in ("init.d", "conf.d"):
                (root / "etc" / d).mkdir(parents=True)
            shipped = set()
            for pkg in pkgs:
                files = apk_files(pkg)
                if not files:
                    self.skipTest(f"no {pkg} in {CACHE}")
                for path, data in files.items():
                    if path.startswith(("etc/init.d/", "etc/conf.d/")) and path.count("/") == 2:
                        (root / path).write_bytes(data)
                        shipped.add(path)
            # as packaged: an absolute link, dangling outside the image
            (root / "etc/init.d/functions.sh").symlink_to("/usr/libexec/rc/sh/functions.sh-not-on-the-host")
            subprocess.run(["cp", "-a", f"{OVERLAY}/etc/.", str(root / "etc")], check=True)
            self.assertIn("etc/init.d/hwclock", shipped)
            self.assertIn("etc/conf.d/hwclock", shipped)
            r = subprocess.run([*self.shell, "-c", block], cwd=REPO, env={**os.environ, "ROOT": str(root)},
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual({p.name for p in (root / "etc/init.d").iterdir()}, DEFAULT | {"functions.sh"})
            self.assertEqual({p.name for p in (root / "etc/conf.d").iterdir()}, set(PACKAGED))
            # a runlevel service missing from the image stops the build
            (root / "etc/init.d/udhcpd").unlink()
            r = subprocess.run([*self.shell, "-c", block], cwd=REPO, env={**os.environ, "ROOT": str(root)},
                               capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("no /etc/init.d/udhcpd in the image", r.stderr)
        # the built image, when there is one with OpenRC in it
        built = REPO / "out/initramfs-root/etc/init.d"
        if (built / "functions.sh").is_symlink():
            self.assertEqual({p.name for p in built.iterdir()}, DEFAULT | {"functions.sh"})

    def test_settings(self):
        conf = (OVERLAY / "etc/rc.conf.d/chef.conf").read_text()
        for setting in ('rc_parallel="NO"', 'rc_interactive="NO"', 'rc_logger="YES"', 'respawn_delay="1s"',
                        "respawn_max=0", 'retry="TERM/5/KILL/2"'):
            self.assertIn("\n" + setting + "\n", conf)
        # the package's rc.conf stays as shipped: no copy in the overlay
        self.assertFalse((OVERLAY / "etc/rc.conf").exists())

    def test_packages(self):
        rootfs = (REPO / "scripts/mkrootfs.sh").read_text()
        pkgs = rootfs.split('PKGS="', 1)[1].split('"', 1)[0].split()
        self.assertIn("openrc", pkgs)
        self.assertEqual({p for p in pkgs if p.endswith("-openrc") and not p.startswith("!")}, set(PACKAGED.values()))


@unittest.skipUnless(shutil.which("busybox"), "needs busybox")
class AshServices(Services):
    shell = ("busybox", "ash")


if __name__ == "__main__":
    unittest.main()
