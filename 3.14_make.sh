#cp /opt/linux/linux-jazz-3.14-tianrong/jazz/arch/arm/boot/zIamge tmpzImage
cat tmpzImage *.dtb > newzImage
mv newzImage zImage
sh make.sh
