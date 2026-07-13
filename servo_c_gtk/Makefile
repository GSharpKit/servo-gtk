all: build-release

mingw: build-release-mingw

build-release: Cargo.lock
	cmake -DCMAKE_BUILD_TYPE=Release -G Ninja -S . -B cmake-build-release
	#cmake --build cmake-build-release --config Release
	ninja -C cmake-build-release -vv

build-release-mingw: Cargo.lock
	mingw64-cmake -DCMAKE_BUILD_TYPE=Release -G Ninja -S . -B cmake-build-release-mingw
	ninja -C cmake-build-release-mingw -vv

install:
	sudo cmake --install cmake-build-release --prefix /usr

install-mingw:
	sudo cmake --install cmake-build-release-mingw

Cargo.lock: Cargo.lock
	cargo generate-lockfile

toolchain:
	rustup toolchain install nightly
	rustup target list --installed
	rustup target add --toolchain nightly x86_64-pc-windows-msvc
	rustup target add x86_64-pc-windows-msvc
	cargo install cargo-xwin	
	ln -s ~/.cache/cargo-xwin/xwin/sdk/lib/um/x86_64/DbgHelp.Lib ~/.cache/cargo-xwin/xwin/sdk/lib/um/x86_64/Dbghelp.lib
	

clean:
	cargo +nightly clean
	-rm Cargo.lock
	-rm -rf cmake-build-release
	-rm -rf cmake-build-debug
	-rm -rf cmake-build-release-mingw
	-rm .ninja_log
	-rm CMakeCache.txt
	-rm -rf CMakeFiles/
	-rm build.ninja
	-rm cmake_install.cmake

