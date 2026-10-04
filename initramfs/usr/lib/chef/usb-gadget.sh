# Sourced by both /init scripts: the stage-2 /init on the root filesystem
# and the stage-1 /init in the boot image (scripts/mkstage1.sh copies this
# file), so the rescue shell and the normal boot expose the same USB network.
# Needs configfs mounted at /sys/kernel/config and a log function.
#
# usb_gadget_up: one NCM ethernet function (phone = 172.16.42.1, DHCP for the
# host). Sets IFACE and writes /run/usb-interface and /run/udhcpd.conf (the
# /etc/udhcpd.conf template with the actual interface; the root is read-only).
# Returns 0 when the interface is up with its address.
usb_gadget_up() {
    SERIAL=chef
    for a in $(cat /proc/cmdline); do
        case "$a" in
            androidboot.serialno=*) SERIAL="${a#*=}" ;;
        esac
    done

    G=/sys/kernel/config/usb_gadget/cyclo
    IFACE=
    if mkdir -p "$G"; then
        echo 0x1d6b > "$G/idVendor"      # Linux Foundation
        echo 0x0104 > "$G/idProduct"     # Multifunction Composite Gadget
        echo 0x0200 > "$G/bcdUSB"
        echo 0x0100 > "$G/bcdDevice"
        mkdir -p "$G/strings/0x409"
        echo "$SERIAL"                > "$G/strings/0x409/serialnumber"
        echo "chef-cyclo"             > "$G/strings/0x409/manufacturer"
        echo "Moto One Power (cyclo)" > "$G/strings/0x409/product"
        mkdir -p "$G/configs/c.1/strings/0x409"
        echo "ncm" > "$G/configs/c.1/strings/0x409/configuration"
        echo 500   > "$G/configs/c.1/MaxPower"
        mkdir -p "$G/functions/ncm.usb0"
        echo 02:43:59:43:4c:01 > "$G/functions/ncm.usb0/dev_addr"   # locally administered
        echo 02:43:59:43:4c:02 > "$G/functions/ncm.usb0/host_addr"
        ln -s "$G/functions/ncm.usb0" "$G/configs/c.1/"

        # The UDC (a800000.dwc3) can take a moment to register.
        i=0
        while [ -z "$(ls /sys/class/udc 2>/dev/null)" ] && [ $i -lt 100 ]; do
            sleep 0.1; i=$((i+1))
        done
        UDC=$(ls /sys/class/udc 2>/dev/null | head -n1)
        if [ -n "$UDC" ] && echo "$UDC" > "$G/UDC"; then
            log "gadget bound to $UDC"
        else
            log "no UDC found, USB gadget not started"
        fi
        IFACE=$(cat "$G/functions/ncm.usb0/ifname" 2>/dev/null)
    else
        log "configfs usb_gadget unavailable"
    fi

    IFACE=${IFACE:-usb0}
    printf '%s\n' "$IFACE" > /run/usb-interface
    sed "s/^interface .*/interface $IFACE/" /etc/udhcpd.conf > /run/udhcpd.conf
    if ip link set "$IFACE" up 2>/dev/null; then
        ip addr add 172.16.42.1/24 dev "$IFACE"
        touch /tmp/udhcpd.leases
        log "$IFACE up, 172.16.42.1/24"
        return 0
    fi
    log "no $IFACE interface"
    return 1
}
