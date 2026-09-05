#!/usr/bin/env bash

set -Eeuo pipefail

# Run everything in the C locale. virsh translates its output and several of
# its fields are parsed below, and it keeps compiler diagnostics matching what
# upstream Haiku documentation shows.
export LC_ALL=C

HAIKU_SOURCE=${HAIKU_SOURCE:-/home/emi/Developer/haiku}
BUILD_DIRECTORY=${BUILD_DIRECTORY:-$HAIKU_SOURCE/generated.x86_64}
ISO_PATH=${ISO_PATH:-$BUILD_DIRECTORY/haiku-nightly-anyboot.iso}
LIBVIRT_URI=${LIBVIRT_URI:-qemu:///system}
LIBVIRT_POOL=${LIBVIRT_POOL:-default}
LIBVIRT_ISO_VOLUME=${LIBVIRT_ISO_VOLUME:-haiku-nightly-anyboot.iso}
VM_NAME=${VM_NAME:-haiku-development}
USB_VENDOR=${USB_VENDOR:-2357}
USB_PRODUCT=${USB_PRODUCT:-0604}
JOBS=${JOBS:-4}

# Debugging. DEBUG is the master switch; the three facilities below can also be
# toggled individually.
DEBUG=${DEBUG:-1}
TRACE_SCRIPT=${TRACE_SCRIPT:-$DEBUG}
BLUETOOTH_DEBUG=${BLUETOOTH_DEBUG:-$DEBUG}
CAPTURE_SERIAL=${CAPTURE_SERIAL:-$DEBUG}
SERIAL_LOG_DIR=${SERIAL_LOG_DIR:-$BUILD_DIRECTORY/logs}
# Jam picks this up automatically, and it lives outside the source tree so it
# can never be committed by accident.
USER_BUILD_CONFIG=${USER_BUILD_CONFIG:-$BUILD_DIRECTORY/UserBuildConfig}

# Password-less SSH into the VM, so it can be driven from here rather than by
# hand. Building it into the image is the only way in: a live image starts with
# no keys and no running daemon.
SSH_ACCESS=${SSH_ACCESS:-1}
SSH_DIRECTORY=${SSH_DIRECTORY:-$BUILD_DIRECTORY/vm-ssh}
SSH_KEY=${SSH_KEY:-$SSH_DIRECTORY/id_ed25519}
SSH_STAGING=${SSH_STAGING:-$BUILD_DIRECTORY/vm-extras}

SKIP_BUILD=${SKIP_BUILD:-0}

usage() {
	cat <<-EOF
		Usage: ${0##*/} [options]

		Builds Haiku, writes the resulting AnyBoot image into the libvirt volume
		backing $VM_NAME, and restarts that VM with the Bluetooth adapter
		attached.

		Options:
		  --skip-build  Deploy the image already in the build directory instead
		                of running jam. Fails if there is no image yet.
		  -h, --help    Show this help and exit.

		Environment:
		  DEBUG=0            Turn all three debugging facilities off at once.
		  TRACE_SCRIPT=0     Do not trace this script with set -x.
		  BLUETOOTH_DEBUG=0  Build the Bluetooth stack without TRACE() output.
		  CAPTURE_SERIAL=0   Do not capture the VM's serial console.
		  SSH_ACCESS=0       Do not build SSH access into the image.
		  JOBS=<n>           Parallel build jobs, currently $JOBS.
		  VM_NAME=<name>     Libvirt domain to deploy to, currently $VM_NAME.
		  SERIAL_LOG_DIR=<d> Where console logs go, currently
		                     $SERIAL_LOG_DIR.
	EOF
}

while (( $# > 0 )); do
	case $1 in
		--skip-build)
			SKIP_BUILD=1
			;;
		-h | --help)
			usage
			exit 0
			;;
		*)
			printf 'Unknown option: %s\n\n' "$1" >&2
			usage >&2
			exit 1
			;;
	esac
	shift
done

if [[ $TRACE_SCRIPT != 0 ]]; then
	PS4='+ ${BASH_SOURCE##*/}:${LINENO}:${FUNCNAME[0]:-main}: '
	set -x
fi

log() {
	printf '\n==> %s\n' "$*"
}

restart_needed=false
recover_vm_on_error() {
	status=$?
	trap - EXIT
	if (( status != 0 )) && $restart_needed; then
		printf '\nDeployment failed after stopping %s; attempting to start it again.\n' \
			"$VM_NAME" >&2
		virsh --connect "$LIBVIRT_URI" start "$VM_NAME" >/dev/null 2>&1 || true
	fi
	exit "$status"
}
trap recover_vm_on_error EXIT

required_commands=(virsh lsusb stat awk)
if (( ! SKIP_BUILD )); then
	required_commands+=(jam)
fi

for command_name in "${required_commands[@]}"; do
	if ! command -v "$command_name" >/dev/null 2>&1; then
		printf 'Required command not found: %s\n' "$command_name" >&2
		exit 1
	fi
done

if [[ ! -d $BUILD_DIRECTORY ]]; then
	printf 'Haiku build directory does not exist: %s\n' "$BUILD_DIRECTORY" >&2
	exit 1
fi

# The Bluetooth stack's TRACE() macros in headers/private/bluetooth/btDebug.h
# are compiled out unless DEBUG is defined, so turn it on for those subtrees
# only. Their objects go to a separate directory, which keeps switching back
# and forth cheap.
# A live image has no host keys and starts no daemon, so both have to be put
# there when it is built. The key pair lives in the build directory and is
# generated once.
configure_ssh_access() {
	if [[ $SSH_ACCESS == 0 ]]; then
		log "SSH access not built into the image"
		return 0
	fi

	if ! command -v ssh-keygen >/dev/null 2>&1; then
		printf 'ssh-keygen not found; SSH access was not set up.\n' >&2
		SSH_ACCESS=0
		return 0
	fi

	mkdir -p "$SSH_DIRECTORY" "$SSH_STAGING"
	chmod 700 "$SSH_DIRECTORY"

	if [[ ! -f $SSH_KEY ]]; then
		log "Generating an SSH key for $VM_NAME"
		ssh-keygen -t ed25519 -N "" -C "haiku-vm" -f "$SSH_KEY" >/dev/null
	fi

	cp "$SSH_KEY.pub" "$SSH_STAGING/authorized_keys"

	# The daemon has to come up after its host keys exist, and on a fresh image
	# they never do, so make them here rather than relying on the package's
	# post-install script having run.
	cat >"$SSH_STAGING/start_sshd.sh" <<-'SCRIPT'
		#!/bin/sh
		# Started by the launch daemon; see the sshd job beside this file.
		settings=$(finddir B_SYSTEM_SETTINGS_DIRECTORY)/ssh
		mkdir -p "$settings"

		for type in ed25519 rsa ecdsa; do
			key="$settings/ssh_host_${type}_key"
			[ -f "$key" ] || ssh-keygen -t "$type" -f "$key" -N "" >/dev/null
		done

		exec /boot/system/bin/sshd -D -e
	SCRIPT

	cat >"$SSH_STAGING/sshd" <<-'JOB'
		service x-vnd.haiku-sshd {
			launch /bin/sh /boot/system/non-packaged/data/launch/start_sshd.sh
			legacy
		}
	JOB

	log "SSH access will be built in, key $SSH_KEY"
}


configure_bluetooth_debug() {
	local begin_marker='# BEGIN rebuild-haiku-vm.sh bluetooth debug'
	local end_marker='# END rebuild-haiku-vm.sh bluetooth debug'
	local preserved=''

	if [[ -f $USER_BUILD_CONFIG ]]; then
		# Keep anything else the file holds, replace only our own block.
		preserved=$(awk -v begin_marker="$begin_marker" \
			-v end_marker="$end_marker" '
			$0 == begin_marker { skipping = 1; next }
			$0 == end_marker { skipping = 0; next }
			!skipping
		' "$USER_BUILD_CONFIG")
	fi

	{
		if [[ -n $preserved ]]; then
			printf '%s\n' "$preserved"
		fi
		if [[ $BLUETOOTH_DEBUG != 0 || $SSH_ACCESS != 0 ]]; then
			printf '%s\n' "$begin_marker"

			if [[ $BLUETOOTH_DEBUG != 0 ]]; then
				local subtree
				for subtree in \
					"src add-ons kernel bluetooth" \
					"src add-ons kernel drivers bluetooth" \
					"src add-ons kernel network protocols bluetooth"; do
					printf 'SetConfigVar DEBUG : HAIKU_TOP %s : 1 : global ;\n' \
						"$subtree"
				done
			fi

			if [[ $SSH_ACCESS != 0 ]]; then
				# The launch daemon reads every "launch" directory it can find,
				# non-packaged included, and sshd looks for authorized keys at
				# config/settings/ssh relative to the home directory.
				printf 'SEARCH on <vm>authorized_keys = %s ;\n' "$SSH_STAGING"
				printf 'SEARCH on <vm>start_sshd.sh = %s ;\n' "$SSH_STAGING"
				printf 'SEARCH on <vm>sshd = %s ;\n' "$SSH_STAGING"
				printf 'AddFilesToHaikuImage home config settings ssh :'
				printf ' <vm>authorized_keys ;\n'
				printf 'AddFilesToHaikuImage system non-packaged data launch :'
				printf ' <vm>start_sshd.sh ;\n'
				printf 'AddFilesToHaikuImage system non-packaged data launch :'
				printf ' <vm>sshd ;\n'
			fi

			printf '%s\n' "$end_marker"
		fi
	} >"$USER_BUILD_CONFIG.new"
	mv "$USER_BUILD_CONFIG.new" "$USER_BUILD_CONFIG"

	if [[ $BLUETOOTH_DEBUG != 0 ]]; then
		log "Bluetooth TRACE() output enabled via $USER_BUILD_CONFIG"
	else
		log "Bluetooth TRACE() output disabled"
	fi
}

if (( SKIP_BUILD )); then
	# The debug configuration only affects compilation, so leave it alone here.
	log "Skipping the build, deploying the image already in $BUILD_DIRECTORY"
else
	configure_ssh_access
	configure_bluetooth_debug

	log "Building Haiku AnyBoot image with $JOBS jobs"
	(
		cd "$BUILD_DIRECTORY"
		jam -q -j"$JOBS" @nightly-anyboot
	)
fi

if [[ ! -s $ISO_PATH ]]; then
	if (( SKIP_BUILD )); then
		printf 'No image to deploy at %s; run once without --skip-build.\n' \
			"$ISO_PATH" >&2
	else
		printf 'Build completed without producing a non-empty ISO: %s\n' \
			"$ISO_PATH" >&2
	fi
	exit 1
fi

if ! lsusb -d "$USB_VENDOR:$USB_PRODUCT" >/dev/null 2>&1; then
	printf 'TP-Link Bluetooth adapter %s:%s is not connected; VM was not restarted.\n' \
		"$USB_VENDOR" "$USB_PRODUCT" >&2
	exit 1
fi

if ! virsh --connect "$LIBVIRT_URI" dominfo "$VM_NAME" >/dev/null 2>&1; then
	printf 'Libvirt VM does not exist: %s\n' "$VM_NAME" >&2
	exit 1
fi

new_capacity=$(stat -c %s "$ISO_PATH")
old_capacity=$(virsh --connect "$LIBVIRT_URI" vol-info \
	--pool "$LIBVIRT_POOL" --bytes "$LIBVIRT_ISO_VOLUME" \
	| awk '/^Capacity:/ { print $2 }')

if [[ ! $old_capacity =~ ^[0-9]+$ ]]; then
	printf 'Could not determine capacity of libvirt volume: %s\n' \
		"$LIBVIRT_ISO_VOLUME" >&2
	exit 1
fi

vm_was_running=false
if [[ $(virsh --connect "$LIBVIRT_URI" domstate "$VM_NAME") == running ]]; then
	vm_was_running=true
fi

log "Stopping $VM_NAME so its virtual CD can be updated safely"
if $vm_was_running; then
	virsh --connect "$LIBVIRT_URI" destroy "$VM_NAME"
fi
restart_needed=true

if (( new_capacity > old_capacity )); then
	log "Growing ISO volume from $old_capacity to $new_capacity bytes"
	virsh --connect "$LIBVIRT_URI" vol-resize \
		--pool "$LIBVIRT_POOL" "$LIBVIRT_ISO_VOLUME" "${new_capacity}B"
elif (( new_capacity < old_capacity )); then
	log "Shrinking ISO volume from $old_capacity to $new_capacity bytes"
	virsh --connect "$LIBVIRT_URI" vol-resize --shrink \
		--pool "$LIBVIRT_POOL" "$LIBVIRT_ISO_VOLUME" "${new_capacity}B"
fi

log "Uploading the new ISO to libvirt storage"
virsh --connect "$LIBVIRT_URI" vol-upload \
	--pool "$LIBVIRT_POOL" "$LIBVIRT_ISO_VOLUME" "$ISO_PATH"

# Haiku enables serial debug output by default, so everything the kernel and
# its add-ons print with dprintf() - including the Bluetooth TRACE() output
# turned on above - arrives on the VM's first serial port.
#
# The pty behind that port belongs to qemu and is not readable by us, so the
# stream is taken from libvirtd through "virsh console" instead. That command
# insists on a controlling terminal, hence the pty allocated by Python, and it
# takes the console over from any other client that holds it.
capture_serial_console() {
	if ! command -v python3 >/dev/null 2>&1; then
		printf 'python3 not found; kernel output is not captured.\n' >&2
		return 0
	fi

	mkdir -p "$SERIAL_LOG_DIR"
	local pid_file=$SERIAL_LOG_DIR/serial-capture.pid

	# Only one client may hold the console, so retire the reader an earlier run
	# may have left behind.
	if [[ -f $pid_file ]]; then
		local previous
		previous=$(<"$pid_file")
		if [[ $previous =~ ^[0-9]+$ ]]; then
			kill "$previous" 2>/dev/null || true
		fi
		rm -f "$pid_file"
	fi

	local log_file
	log_file=$SERIAL_LOG_DIR/serial-$(date +%Y%m%d-%H%M%S).log

	# nohup execs python3 directly, so $! is the reader and it outlives this
	# script. When it is killed the pty master closes and virsh follows.
	nohup python3 -c 'import pty, sys; sys.exit(pty.spawn(sys.argv[1:]))' \
		virsh --connect "$LIBVIRT_URI" console "$VM_NAME" --force \
		>"$log_file" 2>&1 &
	local reader=$!
	disown "$reader" 2>/dev/null || true

	# Give it a moment to fail if the domain has no console at all.
	sleep 1
	if ! kill -0 "$reader" 2>/dev/null; then
		printf 'Could not attach to the console of %s, see %s\n' \
			"$VM_NAME" "$log_file" >&2
		return 0
	fi

	printf '%s\n' "$reader" >"$pid_file"
	ln -sfn "$log_file" "$SERIAL_LOG_DIR/serial-latest.log"

	log "Capturing kernel serial output to $log_file (pid $reader)"
	printf 'Follow it with: tail -f %s\n' "$SERIAL_LOG_DIR/serial-latest.log"
	printf 'Stop it with:   kill %s\n' "$reader"
}

if [[ $CAPTURE_SERIAL != 0 ]]; then
	# Start with the CPUs halted so that the console is attached before the
	# kernel gets to print its first line, then let it run.
	log "Starting $VM_NAME (paused) with TP-Link $USB_VENDOR:$USB_PRODUCT passthrough"
	virsh --connect "$LIBVIRT_URI" start "$VM_NAME" --paused
	capture_serial_console || true
	virsh --connect "$LIBVIRT_URI" resume "$VM_NAME"
else
	log "Starting $VM_NAME with TP-Link $USB_VENDOR:$USB_PRODUCT passthrough"
	virsh --connect "$LIBVIRT_URI" start "$VM_NAME"
fi
restart_needed=false

report_ssh_access() {
	if [[ $SSH_ACCESS == 0 ]]; then
		return 0
	fi

	local mac
	mac=$(virsh --connect "$LIBVIRT_URI" dumpxml "$VM_NAME" \
		| awk -F"'" '/mac address=/ { print $2; exit }')
	if [[ -z $mac ]]; then
		return 0
	fi

	# The guest runs no agent, so its address comes from the lease libvirt
	# handed out rather than from the domain itself.
	local address=''
	local attempt
	for attempt in $(seq 1 30); do
		address=$(virsh --connect "$LIBVIRT_URI" net-dhcp-leases default 2>/dev/null \
			| awk -v mac="$mac" '$0 ~ mac { print $5 }' | cut -d/ -f1 | tail -1)
		if [[ -n $address ]]; then
			break
		fi
		sleep 2
	done

	if [[ -z $address ]]; then
		log "SSH is built in, but $VM_NAME has no lease yet"
		printf 'Once it has one: virsh net-dhcp-leases default\n'
		return 0
	fi

	printf '%s\n' "$address" >"$SSH_DIRECTORY/address"

	log "SSH: ssh -i $SSH_KEY user@$address"
	printf 'Host key checking is pointless for a live image, so add:\n'
	printf '  -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null\n'
}

if [[ $CAPTURE_SERIAL != 0 || $SSH_ACCESS != 0 ]]; then
	report_ssh_access
fi

log "Deployment complete"
virsh --connect "$LIBVIRT_URI" dominfo "$VM_NAME" \
	| awk '/^(Name|State|CPU\(s\)|Used memory):/'
