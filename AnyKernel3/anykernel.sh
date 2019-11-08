# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel setup
# begin properties
properties() { '
kernel.string=Optimus Drunk Kernel by GtrCraft
do.devicecheck=1
do.modules=0
do.cleanup=1
do.cleanuponabort=0
device.name1=potter
device.name2=potter_retail
'; } # end properties

# shell variables
block=/dev/block/bootdevice/by-name/boot;
is_slot_device=0;

## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. tools/ak3-core.sh;

## AnyKernel file attributes
# set permissions/ownership for included ramdisk files
chmod -R 750 $ramdisk/*;
chown -R root:root $ramdisk/*;

## AnyKernel install
dump_boot;

# begin ramdisk changes

backup_file init.rc;

insert_line init.rc "init.optimus.rc" after "import /init.usb.configfs.rc" "import /init.optimus.rc";

# end ramdisk changes

write_boot;
## end install

