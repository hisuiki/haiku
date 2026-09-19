#!/usr/bin/env bash
set -e

VM_IP="${VM_IP:-$(cat /home/emi/Developer/haiku/generated.x86_64/vm-ssh/address 2>/dev/null || echo 192.168.122.139)}"
SSH_KEY="/home/emi/Developer/haiku/generated.x86_64/vm-ssh/id_ed25519"
SSH="ssh -i $SSH_KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 user@$VM_IP"
SCP="scp -i $SSH_KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
GEN="/home/emi/Developer/haiku/generated.x86_64"

# /boot is the live image and is rebuilt on every boot, so nothing deployed
# there survives a restart; the container images live on the second disk
# instead, which only has to be mounted back into place - or made, the first
# time a VM is used.
echo "==> Mounting the persistent volume..."
if ! $SSH "test -d /boot/home/persist/containers"; then
    $SSH "mkdir -p /boot/home/persist"
    PERSIST_DEVICE=$($SSH "for device in /dev/disk/scsi/0/2/0/0 /dev/disk/scsi/0/2/0/raw; do
                               test -e \$device && echo \$device && break
                           done")
    if [ -z "$PERSIST_DEVICE" ]; then
        echo "No second disk to keep the container images on" >&2
        exit 1
    fi
    if ! $SSH "mount -t bfs $PERSIST_DEVICE /boot/home/persist 2>/dev/null"; then
        echo "==> Initialising $PERSIST_DEVICE..."
        $SSH "mkfs -t bfs -q $PERSIST_DEVICE persist >/dev/null \
              && mount -t bfs $PERSIST_DEVICE /boot/home/persist"
    fi
    $SSH "mkdir -p /boot/home/persist/containers/images \
                   /boot/home/persist/containers/instances"
fi

# Anything that ran before the volume was back may have left an empty directory
# where the link belongs; ln would then put the link inside it.
$SSH "test -L /boot/home/data || rmdir /boot/home/data 2>/dev/null; \
      ln -sfn /boot/home/persist /boot/home/data"

echo "==> Preparing directories on VM..."
$SSH "mkdir -p /boot/system/non-packaged/add-ons/kernel/generic \
               /boot/system/non-packaged/bin \
               /boot/system/non-packaged/servers \
               /boot/system/non-packaged/lib \
               /boot/system/non-packaged/add-ons/kernel/file_systems/layers \
               /boot/home/data/containers/images \
               /boot/home/data/containers/instances \
               /boot/home/data/cni/bin"

# Overwriting an executable in place leaves the next exec of it reading a
# mixture of the old and the new file, so each one is removed first.
$SSH "rm -f /boot/system/non-packaged/bin/linux_run \
            /boot/system/non-packaged/bin/haiku-container \
            /boot/system/non-packaged/bin/haiku-cni \
            /boot/system/non-packaged/servers/wayland_server \
            /boot/system/non-packaged/lib/liblinux_runtime.so \
            /boot/system/non-packaged/add-ons/kernel/generic/linux_compat \
            /boot/system/non-packaged/add-ons/kernel/file_systems/linux_procfs \
            /boot/system/non-packaged/add-ons/kernel/file_systems/container_overlay \
            /boot/system/non-packaged/add-ons/kernel/file_systems/layers/container_overlay"

echo "==> Deploying kernel addons and libraries..."
$SCP "$GEN/objects/haiku/x86_64/release/libs/compat/linux/linux_compat" "user@$VM_IP:/boot/system/non-packaged/add-ons/kernel/generic/"
$SCP "$GEN/objects/haiku/x86_64/release/system/linux_runtime_loader/liblinux_runtime.so" "user@$VM_IP:/boot/system/non-packaged/lib/"
$SCP "$GEN/objects/haiku/x86_64/release/add-ons/kernel/file_systems/linux_procfs/linux_procfs" "user@$VM_IP:/boot/system/non-packaged/add-ons/kernel/file_systems/"
$SCP "$GEN/objects/haiku/x86_64/release/add-ons/kernel/file_systems/layers/write_overlay/write_overlay" "user@$VM_IP:/boot/system/non-packaged/add-ons/kernel/file_systems/container_overlay"
$SSH "cp /boot/system/non-packaged/add-ons/kernel/file_systems/container_overlay /boot/system/non-packaged/add-ons/kernel/file_systems/layers/container_overlay && \
      chmod 755 /boot/system/non-packaged/add-ons/kernel/file_systems/container_overlay /boot/system/non-packaged/add-ons/kernel/file_systems/layers/container_overlay"

echo "==> Deploying userland tools..."
$SCP "$GEN/objects/haiku/x86_64/release/servers/wayland/wayland_server" \
     "user@$VM_IP:/boot/system/non-packaged/servers/"
$SSH "chmod 755 /boot/system/non-packaged/servers/wayland_server && \
      mimeset -f /boot/system/non-packaged/servers/wayland_server"
$SCP "$GEN/objects/haiku/x86_64/release/bin/container/haiku-container" \
     "$GEN/objects/haiku/x86_64/release/bin/cni/haiku-cni" \
     "user@$VM_IP:/boot/system/non-packaged/bin/"

$SSH "chmod 755 /boot/system/non-packaged/bin/haiku-container /boot/system/non-packaged/bin/haiku-cni"

if ! $SSH "test -f /boot/home/data/debian-rootfs.tar.gz" 2>/dev/null; then
    echo "==> Deploying debian-rootfs..."
    $SCP /home/emi/Developer/haiku-linux-compat/scratch/debian-rootfs-nohardlinks.tar.gz "user@$VM_IP:/boot/home/data/debian-rootfs.tar.gz"
fi

if ! $SSH "haiku-container images 2>/dev/null | grep -q debian"; then
    echo "==> Importing debian image..."
    $SSH "haiku-container import /boot/home/data/debian-rootfs.tar.gz debian"
fi

if ! $SSH "test -f /boot/home/data/rocky10-rootfs.tar.gz" 2>/dev/null; then
    echo "==> Deploying rocky10-rootfs..."
    $SCP /home/emi/Developer/haiku-linux-compat/scratch/rocky10-rootfs.tar.gz "user@$VM_IP:/boot/home/data/rocky10-rootfs.tar.gz"
fi

if ! $SSH "haiku-container images 2>/dev/null | grep -q rocky"; then
    echo "==> Importing rocky image..."
    $SSH "haiku-container import --size 1024 /boot/home/data/rocky10-rootfs.tar.gz rocky"
fi

echo "==> Deployment complete!"
