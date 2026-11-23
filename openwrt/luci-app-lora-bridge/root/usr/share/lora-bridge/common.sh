# Shared by lora-bridged (daemon) and lora-bridge (CLI). POSIX sh / busybox ash.

RUN=${LB_RUN:-/var/run/lora-bridge}
DEVDIR=$RUN/dev           # shared tmpfs, bind-mounted into the container
PORT_NAME=ttyLORA         # name of the serial port inside that directory
STATE_FILE=$RUN/state
PID_FILE=$RUN/daemon.pid
DOCKER=${LB_DOCKER:-docker}
NL='
'

_lb_append() { eval "$2=\"\${$2:+\${$2}\$NL}\$1\""; }

lb_load_config() {
	if [ -n "$LB_TEST_CONF" ]; then
		# test hook: plain shell assignments instead of UCI
		. "$LB_TEST_CONF"
		return
	fi
	. /lib/functions.sh
	config_load lora-bridge
	config_get_bool ENABLED main enabled 0
	config_get USB_VID main usb_vid 239a
	config_get USB_PID main usb_pid 00b3
	config_get USB_SERIAL main usb_serial ''
	config_get BAUD main baud 115200
	config_get LINK_MODE main link_mode pty
	config_get STOP_DELAY main stop_delay 300
	config_get POLL main poll_interval 2
	config_get C_NAME container name lora-bridge
	config_get_bool C_CREATE container create 1
	config_get C_IMAGE container image ''
	config_get C_COMMAND container command ''
	config_get C_MOUNT container mount_path /dev/lora
	config_get C_NETWORK container network ''
	config_get C_RESTART container restart_policy no
	config_get_bool C_PRIVILEGED container privileged 0
	config_get C_STOP_TIMEOUT container stop_timeout 10
	config_get C_EXTRA container extra_args ''
	C_ENV= C_VOLUME= C_PORT=
	config_list_foreach container env _lb_append C_ENV
	config_list_foreach container volume _lb_append C_VOLUME
	config_list_foreach container port _lb_append C_PORT
}

lb_lower() { echo "$1" | tr 'A-F' 'a-f'; }

# prints "<tty path>|<serial>|<product>|<major:minor>" of the first matching bridge
lb_find_device() {
	local t u vid pid ser prod want_vid want_pid
	if [ -n "$LB_FAKE_DEVICE" ]; then
		# test hook: a file that contains the tty path while the device is "plugged in"
		[ -s "$LB_FAKE_DEVICE" ] && echo "$(cat "$LB_FAKE_DEVICE")|TEST0001|Fake LoRa Bridge|0:0"
		return
	fi
	want_vid=$(lb_lower "$USB_VID")
	want_pid=$(lb_lower "$USB_PID")
	for t in /sys/class/tty/ttyACM* /sys/class/tty/ttyUSB*; do
		[ -e "$t/device" ] || continue
		u=$(readlink -f "$t/device")
		# walk up from the interface to the USB device
		while [ -n "$u" ] && [ "$u" != / ] && [ ! -f "$u/idVendor" ]; do u=${u%/*}; done
		[ -f "$u/idVendor" ] || continue
		vid=$(cat "$u/idVendor"); pid=$(cat "$u/idProduct")
		[ "$vid" = "$want_vid" ] || continue
		[ -z "$want_pid" ] || [ "$pid" = "$want_pid" ] || continue
		ser=$(cat "$u/serial" 2>/dev/null)
		[ -z "$USB_SERIAL" ] || [ "$ser" = "$USB_SERIAL" ] || continue
		prod=$(cat "$u/product" 2>/dev/null)
		echo "/dev/${t##*/}|$ser|$prod|$(cat "$t/dev" 2>/dev/null)"
		return
	done
}

# ---- docker helpers
lb_c_state() { $DOCKER inspect -f '{{.State.Status}}' "$C_NAME" 2>/dev/null; }

lb_c_mount_ok() {
	$DOCKER inspect -f '{{range .Mounts}}{{.Source}}{{"\n"}}{{end}}' "$C_NAME" 2>/dev/null | grep -qx "$DEVDIR"
}

lb_c_create() {
	local IFS_OLD=$IFS x
	[ -n "$C_IMAGE" ] || { echo "no image configured"; return 1; }
	set -- create --name "$C_NAME" --label lora-bridge=1 \
		-v "$DEVDIR:$C_MOUNT:rslave" \
		--device-cgroup-rule 'c 136:* rwm' --device-cgroup-rule 'c 166:* rwm' --device-cgroup-rule 'c 188:* rwm'
	[ -n "$C_NETWORK" ] && set -- "$@" --network "$C_NETWORK"
	[ -n "$C_RESTART" ] && set -- "$@" --restart "$C_RESTART"
	[ "$C_PRIVILEGED" = 1 ] && set -- "$@" --privileged
	IFS=$NL
	for x in $C_ENV; do set -- "$@" -e "$x"; done
	for x in $C_VOLUME; do set -- "$@" -v "$x"; done
	for x in $C_PORT; do set -- "$@" -p "$x"; done
	IFS=$IFS_OLD
	# extra arguments and the command are split on whitespace
	# shellcheck disable=SC2086
	set -- "$@" -e "LORA_BRIDGE_PORT=$C_MOUNT/$PORT_NAME" $C_EXTRA "$C_IMAGE" $C_COMMAND
	$DOCKER "$@"
}

# ---- JSON helpers
lb_jstr() {
	printf '"%s"' "$(printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | tr -d '\r' | tr '\n\t' '  ')"
}
lb_jbool() { [ "$1" = 1 ] && printf true || printf false; }
lb_jnum() { case "$1" in ''|*[!0-9-]*) printf null;; *) printf '%s' "$1";; esac; }