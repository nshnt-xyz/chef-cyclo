/*
 * chef_paths.h - the /run/chef paths C code uses, in one place, so a
 * client and its daemon cannot disagree. Header only.
 *
 * /init makes /run/chef, /run/chef/log, /run/chef/lock and
 * /run/chef/state (0755, root) and /run/chef/screen (root:screen 0775)
 * right after mounting the /run tmpfs; each daemon's service makes its
 * directory under /run/chef (owned by the daemon's user), and the daemon's
 * socket is in it. The layout, owners and modes are in
 * docs/features/runtime-files.md. Every path here keeps its command-line
 * or test override where it had one; shell scripts spell their paths out.
 */
#ifndef CHEF_PATHS_H
#define CHEF_PATHS_H

/* The base: also the settings store's RAM base (<base>/ui/settings until
 * /data is ours, its write lock <base>/lock/ui-settings.lock) and
 * chef-settings' --run default. */
#define CHEF_RUN_DIR		"/run/chef"

/* Local sockets (docs/features/local-sockets.md). libqmi finds the QMUX
 * bridge by the "qmux_socket" in its path, so that name stays. */
#define CHEF_BUTTOND_SOCK	"/run/chef/buttond/buttond.sock"
#define CHEF_BUTTOND_POWEROFF	"/run/chef/buttond/poweroff"	/* power-off hint for chef-state */
#define CHEF_GPS_MANAGER_SOCK	"/run/chef/gps-manager/gps-manager.sock"
#define CHEF_SENSORD_SOCK	"/run/chef/sensord/sensord.sock"
#define CHEF_BLESENSD_SOCK	"/run/chef/blesensd/blesensd.sock"
#define CHEF_QMUX_SOCK		"/run/chef/qmux_socket"

/* The screen handover (tools/fbdev.h, tools/chefui/fbscreen.h): the fb0
 * lock, the panel-lit marker and the system-wide screen-off flag */
#define CHEF_FB0_LOCK		"/run/chef/screen/fb0.lock"
#define CHEF_FB0_LIT		"/run/chef/screen/fb0.lit"
#define CHEF_SCREEN_OFF		"/run/chef/screen/off"

/* Component directories and files in them */
#define CHEF_POWER_DIR		"/run/chef/power"
#define CHEF_SENSORS_REG	"/run/chef/sensord/sns.reg"
#define CHEF_GPS_MANAGER_DIR	"/run/chef/gps-manager"
#define CHEF_SETTINGS_APP_DIR	"/run/chef/chef-settings"	/* stop, args */
/* chef-state's boot clock provenance */
#define CHEF_STATE_TIME		"/run/chef/state/time"

/* modem-up's modem ownership lock (a directory, with "ready" in it) */
#define CHEF_MODEM_UP_LOCK	"/run/chef/lock/modem-up.lock"
#define CHEF_ABSLOT_LOG		"/run/chef/log/abslot.log"

#endif /* CHEF_PATHS_H */
