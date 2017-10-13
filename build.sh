#!/bin/bash

#set -e

## Copy this script inside the kernel directory
KERNEL_DIR=$PWD
KERNEL_TOOLCHAIN=/home/gtrcraft/data/validus/prebuilts/gcc/$linux-x86/aarch64/aarch64-linux-android-4.9/bin
KERNEL_DEFCONFIG=potter_defconfig
DTBTOOL=$KERNEL_DIR/Dtbtool/
JOBS=16
ANY_KERNEL2_DIR=$KERNEL_DIR/AnyKernel2/
FINAL_KERNEL_ZIP=Optimus_Drunk_Potter.zip
# Speed up build process
MAKE="./makeparallel"

BUILD_START=$(date +"%s")
blue='\033[0;34m'
cyan='\033[0;36m'
yellow='\033[0;33m'
red='\033[0;31m'
nocol='\033[0m'

echo "**** Setting Toolchain ****"
export CROSS_COMPILE=$KERNEL_TOOLCHAIN
export ARCH=arm64
export SUBARCH=arm64

# Clean build always lol
echo "**** Cleaning ****"
make clean && make mrproper

echo "**** Kernel defconfig is set to $KERNEL_DEFCONFIG ****"
echo -e "$blue***********************************************"
echo "          BUILDING KERNEL          "
echo -e "***********************************************$nocol"
make $KERNEL_DEFCONFIG
make -j$JOBS

echo -e "$blue***********************************************"
echo "          GENERATING DT.img          "
echo -e "***********************************************$nocol"
$DTBTOOL/dtbToolCM -2 -o $KERNEL_DIR/arch/arm/boot/dtb -s 2048 -p $KERNEL_DIR/scripts/dtc/ $KERNEL_DIR/arch/arm/boot/dts/qcom/

echo "**** Verify zImage & dtb ****"
ls $KERNEL_DIR/arch/arm/boot/zImage
ls $KERNEL_DIR/arch/arm/boot/dtb

#Anykernel 2 time!!
echo "**** Verifying Anyernel2 Directory ****"
ls $ANY_KERNEL2_DIR
echo "**** Removing leftovers ****"
rm -rf $ANY_KERNEL2_DIR/dtb
rm -rf $ANY_KERNEL2_DIR/zImage
rm -rf $ANY_KERNEL2_DIR/$FINAL_KERNEL_ZIP

echo "**** Copying zImage ****"
cp $KERNEL_DIR/arch/arm/boot/zImage $ANY_KERNEL2_DIR/
echo "**** Copying dtb ****"
cp $KERNEL_DIR/arch/arm/boot/dtb $ANY_KERNEL2_DIR/

echo "**** Time to zip up! ****"
cd $ANY_KERNEL2_DIR/
zip -r9 $FINAL_KERNEL_ZIP * -x README $FINAL_KERNEL_ZIP
rm -rf $KERNEL_DIR/Builds/Oreo/$FINAL_KERNEL_ZIP
cp $KERNEL_DIR/AnyKernel2/$FINAL_KERNEL_ZIP /home/gtrcraft/data/kernel/$FINAL_KERNEL_ZIP

echo "**** Good Bye!! ****"
cd $KERNEL_DIR
rm -rf arch/arm/boot/dtb
rm -rf $ANY_KERNEL2_DIR/$FINAL_KERNEL_ZIP
rm -rf AnyKernel2/zImage
rm -rf AnyKernel2/dtb

BUILD_END=$(date +"%s")
DIFF=$(($BUILD_END - $BUILD_START))
echo -e "$yellow Build completed in $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds.$nocol"
