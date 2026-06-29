all: build-release

build-release:
	cargo +nightly build --release
	cmake --build cmake-build-release

install:
	sudo cmake --install cmake-build-release --prefix /usr


install-nightly:
	rustup toolchain install nightly
	

clean:
	cargo +nightly clean
