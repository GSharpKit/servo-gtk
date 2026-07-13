cmake_minimum_required(VERSION 3.20)

project(servo_c_gtk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# Resolve standard install dirs (CMAKE_INSTALL_LIBDIR -> lib64 on Fedora,
# CMAKE_INSTALL_BINDIR -> bin). Configure with -DCMAKE_INSTALL_PREFIX=/usr to
# land the libraries in /usr/lib64.
include(GNUInstallDirs)

pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(GOBJECT_INTROSPECTION REQUIRED gobject-introspection-1.0)

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

# The crate is presented as "libservoshell". Cargo still emits liblibservogtk.so (the
# crate can't be named `servo` without colliding with its `servo` dependency),
# but build.rs stamps it with soname libservoshell.so.0, and we lay down the versioned
# libservoshell.so.0.3.0 / libservoshell.so.0 / libservoshell.so symlinks next to it. These names
# mirror servogtk3's VERSION/SOVERSION below.
set(SERVO_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/target/${CARGO_PROFILE_DIR})
set(CARGO_OUTPUT
        ${SERVO_OUTPUT_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}libservogtk${CMAKE_SHARED_LIBRARY_SUFFIX}
)
# Real, versioned file (dereferenced on install) and the soname/dev symlinks.
set(SERVO_REAL  ${SERVO_OUTPUT_DIR}/libservoshell.so.0.3.0)
set(SERVO_SONAME ${SERVO_OUTPUT_DIR}/libservoshell.so.0)
set(SERVO_LINK  ${SERVO_OUTPUT_DIR}/libservoshell.so)

# Re-run Cargo when any Rust source or the manifest changes. Cargo's own
# incremental build makes a no-op invocation cheap.
file(GLOB_RECURSE SERVO_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/servo/*.rs
)

# Build the cdylib, then create the libservoshell.so{,.0,.0.3.0} symlink chain in the
# Cargo output dir (relative targets keep the chain valid after install too).
add_custom_command(
        OUTPUT ${CARGO_OUTPUT} ${SERVO_REAL} ${SERVO_SONAME} ${SERVO_LINK}
        COMMAND ${RUSTUP_EXECUTABLE} run nightly cargo build ${CARGO_BUILD_FLAGS}
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                ${CMAKE_SHARED_LIBRARY_PREFIX}libservogtk${CMAKE_SHARED_LIBRARY_SUFFIX} libservoshell.so.0.3.0
        COMMAND ${CMAKE_COMMAND} -E create_symlink libservoshell.so.0.3.0 libservoshell.so.0
        COMMAND ${CMAKE_COMMAND} -E create_symlink libservoshell.so.0 libservoshell.so
        WORKING_DIRECTORY ${SERVO_OUTPUT_DIR}
        DEPENDS ${SERVO_SOURCES}
                ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
                ${CMAKE_CURRENT_SOURCE_DIR}/build.rs
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
        --library-path=${CMAKE_CURRENT_BINARY_DIR}
        --output=${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_GIR}
        -I${CMAKE_CURRENT_SOURCE_DIR}
        ${SERVO_GTK_PUBLIC_HEADERS}
        ${SERVO_GTK_SOURCES}
        ${GTK3_CFLAGS}
        DEPENDS servogtk3
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

install(TARGETS servogtk3
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# The Rust cdylib (libservoshell) that servogtk3 links against is built by Cargo, not
# a CMake target, so install it explicitly. install(FILES) copies symlinks
# verbatim, so install the real Cargo output renamed to the versioned name, then
# recreate the soname/dev symlinks in the install tree — the same layout CMake
# would generate for a versioned library target.
install(FILES ${CARGO_OUTPUT}
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RENAME libservoshell.so.0.3.0
)
install(CODE "
    file(CREATE_LINK libservoshell.so.0.3.0 \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libservoshell.so.0\" SYMBOLIC)
    file(CREATE_LINK libservoshell.so.0     \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libservoshell.so\"   SYMBOLIC)
")

install(FILES ${SERVO_GTK_PUBLIC_HEADERS}
        DESTINATION include/servogtk3
)

# C ABI header for the Rust wrapper (libservoshell).
install(FILES servo/servo-webview.h
        DESTINATION include/servogtk3
)

# GIR/typelib are produced by g-ir-scanner, which currently crashes when it
# loads the Servo-backed library standalone. Install them only when present so a
# library install still succeeds without the introspection data.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_GIR}
        DESTINATION share/gir-1.0
        OPTIONAL
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_TYPELIB}
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/girepository-1.0
        OPTIONAL
)

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