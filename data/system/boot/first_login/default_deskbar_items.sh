#!/bin/sh
# Wait for Deskbar to be up and running
attempts=0
while [ $attempts -lt 30 ] && ! /boot/system/bin/roster | grep -q "x-vnd.Be-TSKB"; do
	sleep 0.1
	attempts=$((attempts + 1))
done
sleep 0.2

# install ProcessController, NetworkStatus, PowerStatus & volume control in the Deskbar
/boot/system/apps/ProcessController -deskbar
/boot/system/apps/NetworkStatus --deskbar
/boot/system/apps/PowerStatus --deskbar
/boot/system/bin/desklink --add-volume

# install KeymapSwitcher for certain locales
if [[ `locale -l` =~ ^(ru|uk|be)$ ]]; then
   /boot/system/preferences/KeymapSwitcher --deskbar
fi
