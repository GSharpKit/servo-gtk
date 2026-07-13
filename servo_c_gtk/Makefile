all: build-release

build-release:
	#-mkdir cmake-build-release
	#cargo +nightly build --release
	cmake -DCMAKE_BUILD_TYPE=Release -G Ninja -S . -B cmake-build-release
	cmake --build cmake-build-release --config Release

install:
	sudo cmake --install cmake-build-release --prefix /usr


install-nightly:
	rustup toolchain install nightly
	

clean:
	cargo +nightly clean
	-rm -rf cmake-build-release
	-rm -rf cmake-build-debug
