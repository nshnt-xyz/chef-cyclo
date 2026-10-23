"""The daemons' users and groups and the udev rules that give their device
nodes and sysfs attributes a group (docs/features/users-and-permissions.md).

Static checks of rootfs-overlay/etc/passwd, etc/group and
usr/lib/udev/rules.d/70-chef.rules: the fixed IDs (files on /data keep
numeric owners across images, so they never change once installed), the
group members, and that every group a rule names exists.
"""
from pathlib import Path
import re
import unittest

OVERLAY = Path(__file__).resolve().parents[2] / "rootfs-overlay"

USERS = {"buttond": 200, "powerd": 201, "sensord": 202, "blesensd": 203, "gps": 204, "chefui": 205,
         "polkitd": 206}
# group: (gid, members)
GROUPS = {
    "buttond": (200, {"chefui"}),
    "powerd": (201, set()),
    "sensord": (202, {"chefui"}),
    "blesensd": (203, {"chefui"}),
    "gps": (204, {"chefui"}),
    "chefui": (205, set()),
    "polkitd": (206, set()),
    "qmux": (210, {"gps"}),
    "screen": (211, {"buttond", "chefui"}),
    "vibrator": (212, {"buttond", "powerd", "chefui"}),
    "net_raw": (3004, {"sensord"}),
    "input": (23, {"buttond", "chefui"}),
    "video": (27, {"root", "chefui"}),
    "netdev": (28, {"chefui"}),
    "chrony": (102, {"gps"}),
}


def table(name):
    return [l.split(":") for l in (OVERLAY / "etc" / name).read_text().splitlines() if l.strip()]


class Users(unittest.TestCase):

    def test_accounts(self):
        users = {u[0]: u for u in table("passwd")}
        for name, uid in USERS.items():
            self.assertEqual(":".join(users[name]), f"{name}:x:{uid}:{uid}:{name}:/var/empty:/sbin/nologin")

    def test_groups(self):
        groups = {g[0]: g for g in table("group")}
        for name, (gid, members) in GROUPS.items():
            self.assertEqual(int(groups[name][2]), gid, name)
            self.assertEqual(set(filter(None, groups[name][3].split(","))), members, name)

    def test_ids_unique(self):
        for name, cols in (("passwd", (0, 2)), ("group", (0, 2))):
            rows = table(name)
            for c in cols:
                values = [r[c] for r in rows]
                self.assertEqual(len(values), len(set(values)), f"{name} column {c}")

    def test_members_exist(self):
        # ours (Alpine's stock lines have their own, e.g. kvm without a kvm user)
        users = {u[0] for u in table("passwd")}
        for g in table("group"):
            if g[0] in GROUPS:
                self.assertLessEqual(set(filter(None, g[3].split(","))), users, g[0])

    def test_udev_rules(self):
        rules = (OVERLAY / "usr/lib/udev/rules.d/70-chef.rules").read_text().splitlines()
        body = [l for l in rules if l and not l.startswith("#")]
        self.assertEqual(body, [
            'ACTION=="add", SUBSYSTEM=="ssc", KERNEL=="sensors", GROUP="sensord", MODE="0640"',
            'ACTION=="add", SUBSYSTEM=="leds", KERNEL=="lcd-backlight", RUN+="/bin/chgrp video /sys%p/brightness", '
            'RUN+="/bin/chmod 0664 /sys%p/brightness"',
            'ACTION=="add", SUBSYSTEM=="timed_output", KERNEL=="vibrator", RUN+="/bin/chgrp vibrator /sys%p/enable", '
            'RUN+="/bin/chmod 0664 /sys%p/enable"',
        ])
        self.assertIn("docs/features/users-and-permissions.md", rules[1])
        groups = {g[0] for g in table("group")}
        named = set(re.findall(r'GROUP="(\w+)"', "\n".join(body))) | set(re.findall(r"chgrp (\w+)", "\n".join(body)))
        self.assertLessEqual(named, groups)


if __name__ == "__main__":
    unittest.main()
