VERSION = 0.5.0

all: build-release-linux build-release-mingw

linux: build-release-linux

mingw: build-release-mingw

ucrt: build-release-ucrt

darwinx: build-release-darwinx

build-release-linux: Cargo.lock
	cmake -DCMAKE_BUILD_TYPE=Release -G Ninja -S . -B cmake-build-release
	#cmake --build cmake-build-release --config Release
	ninja -C cmake-build-release -vv

build-release-darwinx: Cargo.lock
	cmake -DCMAKE_BUILD_TYPE=Release -G Ninja -S . -B cmake-build-release-darwinx
	ninja -C cmake-build-release-darwinx -vv

build-release-mingw: Cargo.lock
	mingw64-cmake -DCMAKE_BUILD_TYPE=Release -G Ninja -S . -B cmake-build-release-mingw
	ninja -C cmake-build-release-mingw -vv

build-release-ucrt: Cargo.lock
	ucrt64-cmake -DCMAKE_BUILD_TYPE=Release -G Ninja -S . -B cmake-build-release-ucrt
	ninja -C cmake-build-release-ucrt -vv

install: install-linux install-mingw

install-linux:
	cmake --install cmake-build-release --prefix ${PREFIX}/usr

install-mingw:
	cmake --install cmake-build-release-mingw --prefix ${PREFIX}/usr/x86_64-w64-mingw32/sys-root/mingw/

install-darwinx:
	DESTDIR=${PREFIX} cmake --install cmake-build-release-darwinx

install-ucrt:
	DESTDIR=${PREFIX} cmake --install cmake-build-release-ucrt --config Release


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
	-rm -rf cmake-build-release-ucrt
	-rm -rf cmake-build-release-darwinx
	#-rm .ninja_log
	#-rm CMakeCache.txt
	-rm -rf CMakeFiles/
	#-rm build.ninja
	#-rm cmake_install.cmake
	-rm -rf bottles

rpm:
	make clean
	cd ../ && cp -r servo_c_gtk servogtk-${VERSION} && cd servogtk-${VERSION} && cd ../ && tar cfJ servogtk-${VERSION}.tar.xz servogtk-${VERSION}/ && rm -rf servogtk-${VERSION}
