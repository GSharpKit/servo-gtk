cmake_minimum_required(VERSION 3.20)

project(servo_c_gtk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/libservogtk
        ${GTK3_INCLUDE_DIRS}
)

# The Servo wrapper is built by Cargo (crate-type = cdylib). Rather than rely on
# a pre-built artifact, drive Cargo from CMake so the .so is always built (and
# rebuilt on source changes) before the C library links against it.
find_program(CARGO_EXECUTABLE cargo
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

# Path of the cdylib Cargo produces (liblibservogtk.so on Linux: the "lib"
# prefix/.so suffix are added by the platform, on top of the crate's own
# "libservogtk" lib name).
set(SERVOGTK_RUST_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/target/${CARGO_PROFILE_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}libservogtk${CMAKE_SHARED_LIBRARY_SUFFIX}
)

# Re-run Cargo when any Rust source or the manifest changes. Cargo's own
# incremental build makes a no-op invocation cheap.
file(GLOB_RECURSE SERVOGTK_RUST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/libservogtk/*.rs
)

add_custom_command(
        OUTPUT ${SERVOGTK_RUST_LIB}
        COMMAND ${CARGO_EXECUTABLE} build ${CARGO_BUILD_FLAGS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS ${SERVOGTK_RUST_SOURCES}
                ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
        COMMENT "Building Rust Servo wrapper (cargo build ${CARGO_BUILD_FLAGS})"
        VERBATIM
)

add_custom_target(servogtk_rust DEPENDS ${SERVOGTK_RUST_LIB})

# Link the produced .so by path, and force Cargo to run before this C library is
# linked. The full path also adds target/<profile> to the build-tree RPATH, so
# the loader resolves liblibservogtk.so at runtime.
add_dependencies(servogtk3 servogtk_rust)
target_link_libraries(servogtk3 PUBLIC ${SERVOGTK_RUST_LIB})

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
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
)

install(FILES ${SERVO_GTK_PUBLIC_HEADERS}
        DESTINATION include/servogtk3
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_GIR}
        DESTINATION share/gir-1.0
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${SERVO_GTK_TYPELIB}
        DESTINATION lib/girepository-1.0
)

add_executable(servo_gtk_demo
        demo/servo-gtk-demo.c)

target_include_directories(servo_gtk_demo
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${GTK3_INCLUDE_DIRS})

target_compile_options(servo_gtk_demo
        PRIVATE
        ${GTK3_CFLAGS_OTHER})

target_link_libraries(servo_gtk_demo
        PRIVATE
        servogtk3
        ${GTK3_LIBRARIES})