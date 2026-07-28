make clean
make defconfig
./scripts/kconfig/merge_config.sh -O ./ ./.config ./config-fragment-eagle
make oldconfig
make -j 64 Image && make arrow/socfpga_agilex5_axe5_eagle.dtb
