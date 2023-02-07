git clone https://github.com/DPDK/dpdk
cd dpdk
git checkout releases
meson build -Dexamples=all
cd build
meson configure
ninja
sudo ninja install
sudo ldconfig
echo 1024 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages