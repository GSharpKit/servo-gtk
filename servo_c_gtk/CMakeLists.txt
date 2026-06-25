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

# The Servo wrapper is built by Cargo (crate-type = cdylib) into
# target/<profile>/. Point SERVOGTK_RUST_LIB at that .so to link it, e.g.:
#   cmake -DSERVOGTK_RUST_LIB=/path/to/servo-gtk/target/debug/liblibservogtk.so
find_library(SERVOGTK_RUST_LIB
        NAMES libservogtk servogtk
        HINTS ${CMAKE_CURRENT_SOURCE_DIR}/target/debug
              ${CMAKE_CURRENT_SOURCE_DIR}/target/release
)
if(SERVOGTK_RUST_LIB)
    target_link_libraries(servogtk3 PUBLIC ${SERVOGTK_RUST_LIB})
else()
    message(WARNING "libservogtk (Rust) not found; run `cargo build` first and re-configure. "
                    "The C side will not link until SERVOGTK_RUST_LIB is set.")
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