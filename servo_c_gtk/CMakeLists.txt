cmake_minimum_required(VERSION 3.20)

project(servo_c_gtk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# Resolve standard install dirs (CMAKE_INSTALL_LIBDIR -> lib64 on Fedora,
# CMAKE_INSTALL_BINDIR -> bin). Configure with -DCMAKE_INSTALL_PREFIX=/usr to
# land the libraries in /usr/lib64.
include(GNUInstallDirs)

# GObject-introspection (GIR) is not available/meaningful when cross-compiling
# for Windows with MinGW: g-ir-scanner builds and RUNS a helper against the
# target library, which can't be executed from the cross toolchain. Default the
# GIR build off for MinGW; it stays on for native builds. Can be overridden with
# -DBUILD_GIR=ON/OFF.
if(MINGW)
    option(BUILD_GIR "Build GObject-introspection GIR/typelib" OFF)
else()
    option(BUILD_GIR "Build GObject-introspection GIR/typelib" ON)
endif()

pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
if(BUILD_GIR)
    pkg_check_modules(GOBJECT_INTROSPECTION REQUIRED gobject-introspection-1.0)
endif()

set(SERVO_GTK_API_VERSION 0.2)
set(SERVO_GTK_NAMESPACE ServoGtk)
set(SERVO_GTK_GIR ${SERVO_GTK_NAMESPACE}-${SERVO_GTK_API_VERSION}.gir)
set(SERVO_GTK_TYPELIB ${SERVO_GTK_NAMESPACE}-${SERVO_GTK_API_VERSION}.typelib)

set(SERVO_GTK_PUBLIC_HEADERS
        servo-gtk-web-view.h
)

set(SERVO_GTK_SOURCES
        servo-gtk-web-view.c
)

add_library(servogtk3 SHARED
        ${SERVO_GTK_SOURCES}
        ${SERVO_GTK_PUBLIC_HEADERS}
)

target_include_directories(servogtk3
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/servo
        ${GTK3_INCLUDE_DIRS}
)

# The Servo wrapper is built by Cargo (crate-type = cdylib). Rather than rely on
# a pre-built artifact, drive Cargo from CMake so the .so is always built (and
# rebuilt on source changes) before the C library links against it.
#find_program(CARGO_EXECUTABLE cargo
find_program(RUSTUP_EXECUTABLE rustup
        HINTS $ENV{CARGO_HOME}/bin $ENV{HOME}/.cargo/bin
        REQUIRED
)

# Cargo only emits into target/debug or target/release. Map the CMake build type
# onto those two: anything that isn't a Debug build links the optimized profile.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CARGO_BUILD_FLAGS "")
    set(CARGO_PROFILE_DIR "debug")
else()
    set(CARGO_BUILD_FLAGS "--release")
    set(CARGO_PROFILE_DIR "release")
endif()

# When cross-compiling for Windows with MinGW, drive Cargo at the matching
# Windows Rust target so the crate is built for the same ABI as the C library.
# Cargo then emits into target/<triple>/<profile> instead of target/<profile>.
# Fedora's MSVC-ABI cross environment uses x86_64-pc-windows-msvc; override
# CARGO_TARGET_TRIPLE for a different target/ABI.
if(MINGW)
    set(CARGO_TARGET_TRIPLE "x86_64-pc-windows-msvc"
            CACHE STRING "Rust target triple for the Cargo cdylib build")
    set(CARGO_TARGET_FLAGS --target ${CARGO_TARGET_TRIPLE})
    set(SERVO_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/target/${CARGO_TARGET_TRIPLE}/${CARGO_PROFILE_DIR})
else()
    set(CARGO_TARGET_FLAGS "")
    set(SERVO_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/target/${CARGO_PROFILE_DIR})
endif()

# Re-run Cargo when any Rust source or the manifest changes. Cargo's own
# incremental build makes a no-op invocation cheap.
file(GLOB_RECURSE SERVO_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/servo/*.rs
)

set(CARGO_DEPENDS
        ${SERVO_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
        ${CMAKE_CURRENT_SOURCE_DIR}/build.rs
)

if(WIN32)
    # PE/DLL: no soname or versioned symlink chain. Cargo emits the cdylib named
    # after the crate (libservoshell.dll) plus an import library. servogtk3 links
    # against the import library; the DLL is loaded by file name from the
    # executable's directory at run time. The MSVC ABI names the import library
    # libservoshell.dll.lib; the GNU ABI uses libservoshell.dll.a.
    set(CARGO_OUTPUT ${SERVO_OUTPUT_DIR}/libservoshell.dll)
    if(CARGO_TARGET_TRIPLE MATCHES "msvc")
        set(SERVO_IMPLIB ${SERVO_OUTPUT_DIR}/libservoshell.dll.lib)
    else()
        set(SERVO_IMPLIB ${SERVO_OUTPUT_DIR}/libservoshell.dll.a)
    endif()

    add_custom_command(
            OUTPUT ${CARGO_OUTPUT} ${SERVO_IMPLIB}
            COMMAND ${RUSTUP_EXECUTABLE} run nightly cargo xwin build ${CARGO_BUILD_FLAGS} ${CARGO_TARGET_FLAGS}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            DEPENDS ${CARGO_DEPENDS}
            COMMENT "Building Rust Servo wrapper (cargo build ${CARGO_BUILD_FLAGS} ${CARGO_TARGET_FLAGS})"
            VERBATIM
    )

    add_custom_target(SERVO DEPENDS ${CARGO_OUTPUT} ${SERVO_IMPLIB})
    add_dependencies(servogtk3 SERVO)
    target_link_libraries(servogtk3 PUBLIC ${SERVO_IMPLIB})
else()
    # The crate is presented as "libservoshell". Cargo still emits libservoshell.so
    # (the crate can't be named `servo` without colliding with its `servo`
    # dependency), but build.rs stamps it with soname libservoshell.so.0, and we lay
    # down the versioned libservoshell.so.0.3.0 / libservoshell.so.0 / libservoshell.so
    # symlinks next to it. These names mirror servogtk3's VERSION/SOVERSION below.
    set(CARGO_OUTPUT
            ${SERVO_OUTPUT_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}libservoshell${CMAKE_SHARED_LIBRARY_SUFFIX}
    )
    # Real, versioned file (dereferenced on install) and the soname/dev symlinks.
    set(SERVO_REAL  ${SERVO_OUTPUT_DIR}/libservoshell.so.0.3.0)
    set(SERVO_SONAME ${SERVO_OUTPUT_DIR}/libservoshell.so.0)
    set(SERVO_LINK  ${SERVO_OUTPUT_DIR}/libservoshell.so)

    # Build the cdylib, then create the libservoshell.so{,.0,.0.3.0} symlink chain in
    # the Cargo output dir (relative targets keep the chain valid after install too).
    add_custom_command(
            OUTPUT ${CARGO_OUTPUT} ${SERVO_REAL} ${SERVO_SONAME} ${SERVO_LINK}
            COMMAND ${RUSTUP_EXECUTABLE} run nightly cargo build ${CARGO_BUILD_FLAGS}
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    ${CMAKE_SHARED_LIBRARY_PREFIX}libservoshell${CMAKE_SHARED_LIBRARY_SUFFIX} libservoshell.so.0.3.0
            COMMAND ${CMAKE_COMMAND} -E create_symlink libservoshell.so.0.3.0 libservoshell.so.0
            COMMAND ${CMAKE_COMMAND} -E create_symlink libservoshell.so.0 libservoshell.so
            WORKING_DIRECTORY ${SERVO_OUTPUT_DIR}
            DEPENDS ${CARGO_DEPENDS}
            COMMENT "Building Rust Servo wrapper (cargo build ${CARGO_BUILD_FLAGS}) + libservoshell symlinks"
            VERBATIM
    )

    add_custom_target(SERVO DEPENDS ${CARGO_OUTPUT} ${SERVO_LINK})

    # Link against the libservoshell.so symlink: the linker reads its soname
    # (libservoshell.so.0) and records that as servogtk3's NEEDED entry. The full path
    # also adds target/<profile> to the build-tree RPATH, where libservoshell.so.0
    # resolves at runtime.
    add_dependencies(servogtk3 SERVO)
    target_link_libraries(servogtk3 PUBLIC ${SERVO_LINK})
endif()

target_compile_options(servogtk3
        PRIVATE
        ${GTK3_CFLAGS_OTHER}
)

target_link_libraries(servogtk3
        PUBLIC
        ${GTK3_LIBRARIES}
)

set_target_properties(servogtk3 PROPERTIES
        OUTPUT_NAME "servogtk3"
        VERSION 0.3.0
        SOVERSION 0
        # The installed library finds libservoshell.so.0 next to it in the install
        # libdir. The build tree keeps its own RUNPATH (target/<profile>).
        INSTALL_RPATH "$ORIGIN"
)

if(BUILD_GIR)
# --- Introspection scanner twin -------------------------------------------
# g-ir-scanner links a helper program against the target library and RUNS it to
# dump GObject metadata. Linking the real servogtk3 pulls in libservoshell (the
# Servo Rust runtime), whose jemalloc load-time constructor SIGSEGVs inside the
# scanner's minimal dependency closure (it loads fine in the full demo/CLR
# process). Introspection never instantiates a webview — it only reads the
# ServoGtkWebView type/properties/signals from class_init, which never call the
# servo_webview_* FFI. So build a twin of the library from the same C sources
# plus no-op FFI stubs (servo-webview-scan-stub.c) with NO dependency on
# libservoshell, and point the scanner at it. It carries the same OUTPUT_NAME
# and soname (libservogtk3.so.0) as the real library, so the shared-library
# recorded in the GIR is correct and consumers load the real runtime at run
# time. Kept in a separate output dir so it never shadows the real servogtk3.so.
add_library(servogtk3-scan SHARED
        ${SERVO_GTK_SOURCES}
        servo-webview-scan-stub.c
        ${SERVO_GTK_PUBLIC_HEADERS}
)

target_include_directories(servogtk3-scan
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/servo
        ${GTK3_INCLUDE_DIRS}
)

target_compile_options(servogtk3-scan PRIVATE ${GTK3_CFLAGS_OTHER})
target_link_libraries(servogtk3-scan PRIVATE ${GTK3_LIBRARIES})

set(SERVO_GTK_SCAN_DIR ${CMAKE_CURRENT_BINARY_DIR}/scan)
set_target_properties(servogtk3-scan PROPERTIES
        OUTPUT_NAME servogtk3
        VERSION 0.3.0
        SOVERSION 0
        LIBRARY_OUTPUT_DIRECTORY ${SERVO_GTK_SCAN_DIR}
)

add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_GIR}
        COMMAND
        ${GOBJECT_INTROSPECTION_PREFIX}/bin/g-ir-scanner
        --namespace=${SERVO_GTK_NAMESPACE}
        --nsversion=${SERVO_GTK_API_VERSION}
        --warn-all
        --identifier-prefix=ServoGtk
        --symbol-prefix=servo_gtk
        --include=Gtk-3.0
        --library=servogtk3
        --library-path=${SERVO_GTK_SCAN_DIR}
        --output=${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_GIR}
        -I${CMAKE_CURRENT_SOURCE_DIR}
        ${SERVO_GTK_PUBLIC_HEADERS}
        ${SERVO_GTK_SOURCES}
        ${GTK3_CFLAGS}
        DEPENDS servogtk3-scan
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
)

add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_TYPELIB}
        COMMAND
        ${GOBJECT_INTROSPECTION_PREFIX}/bin/g-ir-compiler
        ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_GIR}
        --output=${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_TYPELIB}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_GIR}
        VERBATIM
)

add_custom_target(gir ALL
        DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_GIR}
        ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_TYPELIB}
)
endif() # BUILD_GIR

install(TARGETS servogtk3
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# The Rust cdylib (libservoshell) that servogtk3 links against is built by Cargo, not
# a CMake target, so install it explicitly.
if(WIN32)
    # A Windows DLL is loaded by file name from the executable's directory, so
    # ship it in the runtime (bin) dir alongside servogtk3.dll and the demo.
    install(FILES ${CARGO_OUTPUT}
            DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
else()
    # install(FILES) copies symlinks verbatim, so install the real Cargo output
    # renamed to the versioned name, then recreate the soname/dev symlinks in the
    # install tree — the same layout CMake would generate for a versioned library.
    install(FILES ${CARGO_OUTPUT}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RENAME libservoshell.so.0.3.0
    )
    install(CODE "
        file(CREATE_LINK libservoshell.so.0.3.0 \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libservoshell.so.0\" SYMBOLIC)
        file(CREATE_LINK libservoshell.so.0     \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libservoshell.so\"   SYMBOLIC)
    ")
endif()

install(FILES ${SERVO_GTK_PUBLIC_HEADERS}
        DESTINATION include/servogtk3
)

# C ABI header for the Rust wrapper (libservoshell).
install(FILES servo/servo-webview.h
        DESTINATION include/servogtk3
)

# GIR/typelib are produced by g-ir-scanner against the servogtk3-scan twin (see
# above). Kept OPTIONAL so a library install still succeeds even if the
# introspection data was not built.
if(BUILD_GIR)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_GIR}
            DESTINATION share/gir-1.0
            OPTIONAL
    )

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_TYPELIB}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/girepository-1.0
            OPTIONAL
    )
endif()

add_executable(servo-gtk-demo
        demo/servo-gtk-demo.c)

target_include_directories(servo-gtk-demo
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${GTK3_INCLUDE_DIRS})

target_compile_options(servo-gtk-demo
        PRIVATE
        ${GTK3_CFLAGS_OTHER})

target_link_libraries(servo-gtk-demo
        PRIVATE
        servogtk3
        ${GTK3_LIBRARIES})

# Installed to <prefix>/bin, it finds servogtk3 + libservoshll in the install libdir
# via a relative rpath.
set_target_properties(servo-gtk-demo PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
)

install(TARGETS servo-gtk-demo
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)