#!/bin/sh
set -e

cd /usr/src/linux
make -j4 bzImage
sudo cp /usr/src/linux/arch/x86_64/boot/bzImage /boot/vmlinuz-4.15.4
make -j4 modules
sudo make -j4 modules_install
sudo update-initramfs -k 4.15.4 -u
while true; do
    read -p "Do you want to reboot now ? [y/n] " yn
        case $yn in
            [Yy]* ) echo "Rebooting..";sudo reboot;break;;
            [Nn]* ) echo "Kernel will be reloaded at next reboot.";break;;
            * ) echo "Please answer yes or no.";;
        esac
done
