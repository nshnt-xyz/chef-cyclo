# Live testing on the phone

[Project overview](../README.md) · [Build and boot](building.md) · [Device reference](device.md)

Two host scripts drive every live test: `scripts/phone-boot.sh` boots an image
and `scripts/phone.py` talks to the booted image's USB shell. `fastboot boot`
flashes nothing; a normal reboot boots the image installed in `boot_a` (ours
since 2026-10-04, see [standalone boot](next-steps/storage-and-boot.md#standalone-boot)),
not Android. Host needs: `fastboot`, `telnet` and python3-pexpect (`adb` only
for the old Android path).

## Boot an image

```sh
scripts/phone-boot.sh                          # out/boot.img
scripts/phone-boot.sh out/boot-test.img        # any image
EXPECT=<sha256> scripts/phone-boot.sh IMG      # refuse a different build
```

It gets the phone into fastboot from wherever it is (already in fastboot, one of
our images via `btprobe restart bootloader` over telnet, or stock Android via
`adb reboot bootloader` if it is ever reinstalled), runs `fastboot boot`, and waits until 172.16.42.1
answers (about 20 s after `fastboot boot`). If the first `fastboot boot` fails,
as it can right after `adb reboot bootloader` (garbled `getvar` replies,
`FAILED (remote: 'unknown command')`), it runs `fastboot reboot bootloader` and
retries once. If that fails too, move the cable to another USB port.

Build test images under another name (`OUT=out/boot-foo.img scripts/mkboot.sh`)
so `out/boot.img` stays the known-good baseline. A test image marks `boot_a`
successful 30 s after boot like the installed one (`abslot`); while `boot_a` is
not yet marked (right after a flash), every boot, `fastboot boot` included,
takes one of its seven retries.

## Talk to the booted image

```sh
scripts/phone.py run 'uname -a; cat /proc/uptime'   # exit status = remote status
echo 'dmesg | tail' | scripts/phone.py run           # command on stdin
scripts/phone.py -t 300 run 'long-command'           # timeout in s (default 60)
scripts/phone.py push build/tool -d /run/test        # verified by sha256
scripts/phone.py pull /run/evidence -o ev.tgz        # byte-exact tar.gz
scripts/phone.py stream 'cat /dev/kmsg' kmsg.txt     # until the phone goes away
scripts/phone.py --log DIR run '...'                 # keep a session transcript
```

`telnet 172.16.42.1` gives the same passwordless root shell interactively.
Push a rebuilt static binary instead of rebooting when iterating on a tool;
`/run` and `/tmp` are RAM.

## Gotchas

- **Background jobs die with the session.** Closing telnet SIGHUPs foreground
  and `&` jobs. Start anything that must outlive the command with
  `setsid CMD </dev/null >/run/CMD.out 2>&1 &`.
- **`pull` reads regular files.** `/proc` and `/sys` entries come through tar
  as zero bytes; read them with `run 'cat ...'`.
- **busybox, not GNU.** `base64` has no `-w` (the scripts wrap and rejoin);
  `/bin` holds only busybox, busybox-extras and sh, with applet links made by
  `/init` at boot.
- **Evidence under `/run` and `/tmp` is in RAM.** Pull it before rebooting. A phase 2 image persists the reviewed BlueZ and NetworkManager directories under `/data`; see [storage](features/storage.md).
- **`poweroff` with USB attached comes back.** The cable re-powers the phone in
  about 24 s (`androidboot.mode=charger`), and our full image boots. Unplug
  first if it should stay off.
- **The panel is not visible from here.** Display, touch and button checks need
  someone watching the phone. Announce a window ("tap now for 25 s"), then
  run the check (`fbtouch show -t 25 -r`, `chefui-demo`), and ask what they saw.
  For guided multi-step runs use numbered `STEP k/N` lines and give at least
  8 s to move and 10 s to hold.
- **Do not `cat /proc/nvt_fw_version`** (or touch IC sysfs) while the panel is
  off: it does I2C reads on the unpowered controller and breaks touch until the
  next resume.

## Evidence

Keep raw transcripts and pulls of a live run in one directory per run, for example
`~/chef-cyclo-evidence/<topic>-<UTC stamp>/` (outside the repository), and copy
the parts worth keeping into `logs/` with a [build log](build-log.md) entry.
Record the image hash (`phone-boot.sh` prints it).

Back to the installed image: `scripts/phone.py run reboot`. Into fastboot
without a working image: hold Power with VolDown for about 9 s. Android is
retired; its restore route is under
[images to restore](next-steps/storage-and-boot.md#images-to-restore).
