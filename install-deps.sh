sudo apt-get install -y libhyperscan-dev libcork-dev
sudo apt-get install -y build-essential cmake cmake-curses-gui pkg-config check

curl https://www.spamhaus.org/drop/drop.txt > drop.txt

git clone https://github.com/shadowsocks/ipset
cd ipset
sed 's/add_definitions(-Wall -Werror)//' -i CMakeLists.txt
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_C_FLAGS=-pthread ..
make
sudo make install
