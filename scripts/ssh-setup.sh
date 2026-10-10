#!/bin/sh
# Host side of the phone's SSH (docs/features/ssh.md):
#   1. create the dedicated key ~/.ssh/chef-cyclo_ed25519 if it is missing,
#      with no passphrase so scripts and agents can use it non-interactively
#      (protect the file like any unencrypted key, or add a passphrase with
#      `ssh-keygen -p -f ~/.ssh/chef-cyclo_ed25519` and use an agent)
#   2. add its public half to secrets/ssh/authorized_keys (gitignored), the
#      file scripts/mkinitramfs.sh bakes into the image, unless already there
#   3. print an ~/.ssh/config snippet; ~/.ssh/config itself is not touched.
# The next image build carries the key; boot or install it to use it.
#   KEY=PATH scripts/ssh-setup.sh   another key (default ~/.ssh/chef-cyclo_ed25519)
set -eu
cd "$(dirname "$0")/.."
KEY=${KEY:-$HOME/.ssh/chef-cyclo_ed25519}
AUTH=secrets/ssh/authorized_keys
command -v ssh-keygen >/dev/null || { echo "ssh-keygen (OpenSSH client) is required" >&2; exit 1; }
umask 077
if [ -e "$KEY" ]; then
    echo "key exists: $KEY"
else
    mkdir -p "$(dirname "$KEY")"
    ssh-keygen -q -t ed25519 -N '' -C "chef-cyclo $(id -un)@$(hostname)" -f "$KEY"
    echo "created $KEY (no passphrase)"
fi
[ -s "$KEY.pub" ] || { echo "no public key $KEY.pub" >&2; exit 1; }
mkdir -p secrets/ssh
chmod 700 secrets secrets/ssh
pub=$(cat "$KEY.pub")
blob=$(printf '%s\n' "$pub" | awk '{print $2}')
if [ -f "$AUTH" ] && awk -v b="$blob" '$2==b {f=1} END {exit !f}' "$AUTH"; then
    echo "already in $AUTH"
else
    printf '%s\n' "$pub" >> "$AUTH"
    echo "added $KEY.pub to $AUTH"
fi
chmod 600 "$AUTH"
python3 scripts/ssh-keys.py check "$AUTH"
git check-ignore -q "$AUTH" || { echo "$AUTH is not gitignored; refusing to leave it" >&2; exit 1; }
cat <<EOF

Rebuild the image to carry the key. ~/.ssh/config snippet (not installed;
HostKeyAlias keeps one known_hosts entry whatever address the phone has):

Host chef chef-usb
    User root
    IdentityFile $KEY
    IdentitiesOnly yes
    UserKnownHostsFile ~/.ssh/chef-cyclo_known_hosts
    StrictHostKeyChecking accept-new
    HostKeyAlias chef-cyclo
Host chef
    # the Wi-Fi address the panel shows ("chef Wi-Fi: ... ssh root@ADDR")
    HostName 192.168.0.116
Host chef-usb
    HostName 172.16.42.1

scripts/phone.py --ssh ADDR run 'uname -a' uses the same key and known_hosts
file without the snippet.
EOF
