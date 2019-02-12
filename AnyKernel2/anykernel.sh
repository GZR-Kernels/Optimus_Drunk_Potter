# AnyKernel2 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel setup
# begin properties
properties() {
kernel.string=Optimus Drunk Kernel by GtrCraft
do.devicecheck=1
do.modules=0
do.cleanup=1
do.cleanuponabort=0
device.name1=potter
device.name2=potter_retail
} # end properties

# shell variables
block=/dev/block/bootdevice/by-name/boot;
is_slot_device=0;


## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. /tmp/anykernel/tools/ak2-core.sh;


## AnyKernel permissions
# set permissions for included ramdisk files
chmod -R 750 $ramdisk/*;
chown -R root:root $ramdisk/*;

## AnyKernel install
dump_boot;

# begin ramdisk changes

mount -o rw,remount -t auto /system;
mount -o rw,remount -t auto /vendor 2>/dev/null;

backup_file vendor/etc/init/hw/init.qcom.rc;

replace_section vendor/etc/init/hw/init.qcom.rc "    write /dev/cpuset/foreground/cpus 0-3,6-7" "    write /dev/cpuset/system-background/cpus 0-3" "    write /dev/cpuset/foreground/cpus 0-7\n    write /dev/cpuset/background/cpus 0-7\n    write /dev/cpuset/system-background/cpus 0-7";

replace_section vendor/etc/init/hw/init.qcom.rc "service vendor.energy-awareness /system/vendor/bin/energy-awareness" "    oneshot" "#service vendor.energy-awareness /system/vendor/bin/energy-awareness\n#    class main\n#    user root\n#    group system\n#    oneshot";

replace_section vendor/etc/init/hw/init.qcom.rc "service cnss-daemon /system/vendor/bin/cnss-daemon -n -l" "   stop cnss-daemon" "#service cnss-daemon /system/vendor/bin/cnss-daemon -n -l\n#    class late_start\n#    user system\n#    group system inet net_admin wifi\n#    capabilities NET_ADMIN\n#on property:sys.powerctl=*\n#    stop cnss-daemon";

mount -o ro,remount -t auto /system;
mount -o ro,remount -t auto /vendor 2>/dev/null;

backup_file init.rc;

insert_line init.rc "init.optimus.rc" after "import /init.usb.configfs.rc" "import /init.optimus.rc";

# end ramdisk changes

write_boot;

## end install

