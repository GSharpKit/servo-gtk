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

# Both the GTK3 and GTK4 variants of the library are built together, each
# against its own GTK. The two libraries share the same public API and the same
# Rust runtime (libservoshell); only the GTK internals differ.
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(GTK4 REQUIRED gtk4)
if(BUILD_GIR)
    pkg_check_modules(GOBJECT_INTROSPECTION REQUIRED gobject-introspection-1.0)
endif()

# Library version (full VERSION x.y.z) and ABI major (SOVERSION). These drive
# each servogtkN's VERSION/SOVERSION and the libservoshell.so symlink chain below.
set(SERVO_VERSION 0.3.0)
set(SERVO_SOVERSION 0)

# GObject-introspection namespace is shared; only the nsversion differs per GTK
# major (ServoGtk-3.0 / ServoGtk-4.0).
set(SERVO_GTK_NAMESPACE ServoGtk)

# ==========================================================================
# Rust Servo wrapper (libservoshell), built once and shared by both libraries.
# ==========================================================================
# The Servo wrapper is built by Cargo (crate-type = cdylib). Rather than rely on
# a pre-built artifact, drive Cargo from CMake so the .so is always built (and
# rebuilt on source changes) before the C libraries link against it.
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
    # after the crate (libservoshell.dll) plus an import library. The libraries
    # link against the import library; the DLL is loaded by file name from the
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
    # What the C libraries link against (import library on Windows).
    set(SERVO_LINK_LIB ${SERVO_IMPLIB})
else()
    # The crate is presented as "libservoshell". Cargo still emits libservoshell.so
    # (the crate can't be named `servo` without colliding with its `servo`
    # dependency), but build.rs stamps it with soname libservoshell.so.${SERVO_SOVERSION},
    # and we lay down the versioned libservoshell.so.${SERVO_VERSION} /
    # libservoshell.so.${SERVO_SOVERSION} / libservoshell.so symlinks next to it. These
    # names mirror each servogtkN's VERSION/SOVERSION below.
    set(CARGO_OUTPUT
            ${SERVO_OUTPUT_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}libservoshell${CMAKE_SHARED_LIBRARY_SUFFIX}
    )
    # Real, versioned file (dereferenced on install) and the soname/dev symlinks.
    set(SERVO_REAL  ${SERVO_OUTPUT_DIR}/libservoshell.so.${SERVO_VERSION})
    set(SERVO_SONAME ${SERVO_OUTPUT_DIR}/libservoshell.so.${SERVO_SOVERSION})
    set(SERVO_LINK  ${SERVO_OUTPUT_DIR}/libservoshell.so)

    # Build the cdylib, then create the libservoshell.so symlink chain in the Cargo
    # output dir (relative targets keep the chain valid after install too).
    add_custom_command(
            OUTPUT ${CARGO_OUTPUT} ${SERVO_REAL} ${SERVO_SONAME} ${SERVO_LINK}
            COMMAND ${RUSTUP_EXECUTABLE} run nightly cargo build ${CARGO_BUILD_FLAGS}
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    ${CMAKE_SHARED_LIBRARY_PREFIX}libservoshell${CMAKE_SHARED_LIBRARY_SUFFIX} libservoshell.so.${SERVO_VERSION}
            COMMAND ${CMAKE_COMMAND} -E create_symlink libservoshell.so.${SERVO_VERSION} libservoshell.so.${SERVO_SOVERSION}
            COMMAND ${CMAKE_COMMAND} -E create_symlink libservoshell.so.${SERVO_SOVERSION} libservoshell.so
            WORKING_DIRECTORY ${SERVO_OUTPUT_DIR}
            DEPENDS ${CARGO_DEPENDS}
            COMMENT "Building Rust Servo wrapper (cargo build ${CARGO_BUILD_FLAGS}) + libservoshell symlinks"
            VERBATIM
    )

    add_custom_target(SERVO DEPENDS ${CARGO_OUTPUT} ${SERVO_LINK})

    # Link against the libservoshell.so symlink: the linker reads its soname
    # (libservoshell.so.0) and records that as each servogtkN's NEEDED entry. The
    # full path also adds target/<profile> to the build-tree RPATH, where
    # libservoshell.so.0 resolves at runtime.
    set(SERVO_LINK_LIB ${SERVO_LINK})
endif()

# ==========================================================================
# Per-GTK-major library, introspection twin, demo and install rules.
# ==========================================================================
# Builds one variant of the library (servogtk<MAJOR>) plus its demo and, when
# BUILD_GIR is on, a scanner twin and its GIR/typelib. Called once per GTK major
# so the GTK3 and GTK4 variants build side by side.
#
# Arguments:
#   MAJOR       - GTK major version, e.g. 3 or 4 (names all outputs).
#   API_VERSION - GIR nsversion, e.g. 3.0 or 4.0.
#   GTK_PREFIX  - pkg_check_modules prefix to read flags from (GTK3 / GTK4).
#   GIR_INCLUDE - GObject-introspection dependency, e.g. Gtk-3.0 / Gtk-4.0.
#   SOURCES     - C source files.
#   HEADERS     - public C headers.
function(add_servo_gtk_library)
    set(oneValueArgs MAJOR API_VERSION GTK_PREFIX GIR_INCLUDE)
    set(multiValueArgs SOURCES HEADERS)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(lib servogtk${ARG_MAJOR})
    set(gir ${SERVO_GTK_NAMESPACE}-${ARG_API_VERSION}.gir)
    set(typelib ${SERVO_GTK_NAMESPACE}-${ARG_API_VERSION}.typelib)

    # GTK flags, resolved from the pkg_check_modules prefix (GTK3_* / GTK4_*).
    set(gtk_include_dirs  ${${ARG_GTK_PREFIX}_INCLUDE_DIRS})
    set(gtk_libraries     ${${ARG_GTK_PREFIX}_LIBRARIES})
    set(gtk_cflags_other  ${${ARG_GTK_PREFIX}_CFLAGS_OTHER})
    set(gtk_cflags        ${${ARG_GTK_PREFIX}_CFLAGS})

    # g-ir-scanner's option parser only accepts preprocessor-style flags
    # (-I/-D/-U) among the compiler flags. GTK4's cflags carry arch/codegen flags
    # (-mfpmath=sse / -msse / -msse2, pulled in via graphene) that make it error
    # with "no such option: -m"; those are irrelevant to introspection. Keep only
    # the flags the scanner understands.
    set(gir_cflags ${gtk_cflags})
    list(FILTER gir_cflags INCLUDE REGEX "^-[IDU]")

    # --- The library --------------------------------------------------------
    add_library(${lib} SHARED
            ${ARG_SOURCES}
            ${ARG_HEADERS}
    )

    target_include_directories(${lib}
            PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/servo
            ${gtk_include_dirs}
    )

    target_compile_options(${lib} PRIVATE ${gtk_cflags_other})

    add_dependencies(${lib} SERVO)
    target_link_libraries(${lib}
            PUBLIC
            ${SERVO_LINK_LIB}
            ${gtk_libraries}
    )

    set_target_properties(${lib} PROPERTIES
            OUTPUT_NAME ${lib}
            VERSION ${SERVO_VERSION}
            SOVERSION ${SERVO_SOVERSION}
            # The installed library finds libservoshell.so.0 next to it in the
            # install libdir. The build tree keeps its own RUNPATH (target/<profile>).
            INSTALL_RPATH "$ORIGIN"
    )

    if(BUILD_GIR)
        # --- Introspection scanner twin ------------------------------------
        # g-ir-scanner links a helper program against the target library and RUNS
        # it to dump GObject metadata. Linking the real library pulls in
        # libservoshell (the Servo Rust runtime), whose jemalloc load-time
        # constructor SIGSEGVs inside the scanner's minimal dependency closure (it
        # loads fine in the full demo/CLR process). Introspection never
        # instantiates a webview — it only reads the ServoGtkWebView
        # type/properties/signals from class_init, which never call the
        # servo_webview_* FFI. So build a twin of the library from the same C
        # sources plus no-op FFI stubs (servo-webview-scan-stub.c) with NO
        # dependency on libservoshell, and point the scanner at it. It carries the
        # same OUTPUT_NAME and soname as the real library, so the shared-library
        # recorded in the GIR is correct and consumers load the real runtime at
        # run time. Kept in a separate output dir so it never shadows the real
        # library.
        add_library(${lib}-scan SHARED
                ${ARG_SOURCES}
                servo-webview-scan-stub.c
                ${ARG_HEADERS}
        )

        target_include_directories(${lib}-scan
                PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/servo
                ${gtk_include_dirs}
        )

        target_compile_options(${lib}-scan PRIVATE ${gtk_cflags_other})
        target_link_libraries(${lib}-scan PRIVATE ${gtk_libraries})

        set(scan_dir ${CMAKE_CURRENT_BINARY_DIR}/scan${ARG_MAJOR})
        set_target_properties(${lib}-scan PROPERTIES
                OUTPUT_NAME ${lib}
                VERSION ${SERVO_VERSION}
                SOVERSION ${SERVO_SOVERSION}
                LIBRARY_OUTPUT_DIRECTORY ${scan_dir}
        )

        add_custom_command(
                OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${gir}
                COMMAND
                ${GOBJECT_INTROSPECTION_PREFIX}/bin/g-ir-scanner
                --namespace=${SERVO_GTK_NAMESPACE}
                --nsversion=${ARG_API_VERSION}
                --warn-all
                --identifier-prefix=ServoGtk
                --symbol-prefix=servo_gtk
                --include=${ARG_GIR_INCLUDE}
                --library=${lib}
                --library-path=${scan_dir}
                --output=${CMAKE_CURRENT_BINARY_DIR}/${gir}
                -I${CMAKE_CURRENT_SOURCE_DIR}
                ${ARG_HEADERS}
                ${ARG_SOURCES}
                ${gir_cflags}
                DEPENDS ${lib}-scan
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                VERBATIM
        )

        add_custom_command(
                OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${typelib}
                COMMAND
                ${GOBJECT_INTROSPECTION_PREFIX}/bin/g-ir-compiler
                ${CMAKE_CURRENT_BINARY_DIR}/${gir}
                --output=${CMAKE_CURRENT_BINARY_DIR}/${typelib}
                DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${gir}
                VERBATIM
        )

        add_custom_target(gir-gtk${ARG_MAJOR} ALL
                DEPENDS
                ${CMAKE_CURRENT_BINARY_DIR}/${gir}
                ${CMAKE_CURRENT_BINARY_DIR}/${typelib}
        )
    endif() # BUILD_GIR

    # --- Install ------------------------------------------------------------
    install(TARGETS ${lib}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(FILES ${ARG_HEADERS}
            DESTINATION include/${lib}
    )

    # C ABI header for the Rust wrapper (libservoshell).
    install(FILES servo/servo-webview.h
            DESTINATION include/${lib}
    )

    # GIR/typelib are produced by g-ir-scanner against the scanner twin (see
    # above). Kept OPTIONAL so a library install still succeeds even if the
    # introspection data was not built.
    if(BUILD_GIR)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${gir}
                DESTINATION share/gir-1.0
                OPTIONAL
        )

        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${typelib}
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/girepository-1.0
                OPTIONAL
        )
    endif()

    # --- Demo ---------------------------------------------------------------
    add_executable(${lib}-demo
            demo/servo-gtk${ARG_MAJOR}-demo.c)

    target_include_directories(${lib}-demo
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${gtk_include_dirs})

    target_compile_options(${lib}-demo
            PRIVATE
            ${gtk_cflags_other})

    target_link_libraries(${lib}-demo
            PRIVATE
            ${lib}
            ${gtk_libraries})

    # Installed to <prefix>/bin, it finds servogtkN + libservoshell in the install
    # libdir via a relative rpath.
    set_target_properties(${lib}-demo PROPERTIES
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )

    install(TARGETS ${lib}-demo
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endfunction()

add_servo_gtk_library(
        MAJOR       3
        API_VERSION 3.0
        GTK_PREFIX  GTK3
        GIR_INCLUDE Gtk-3.0
        SOURCES     servo-gtk3-view.c
        HEADERS     servo-gtk3-view.h
)

add_servo_gtk_library(
        MAJOR       4
        API_VERSION 4.0
        GTK_PREFIX  GTK4
        GIR_INCLUDE Gtk-4.0
        SOURCES     servo-gtk4-view.c
        HEADERS     servo-gtk4-view.h
)

# The Rust cdylib (libservoshell) that both libraries link against is built by
# Cargo, not a CMake target, and is shared, so install it once.
if(WIN32)
    # A Windows DLL is loaded by file name from the executable's directory, so
    # ship it in the runtime (bin) dir alongside servogtkN.dll and the demos.
    install(FILES ${CARGO_OUTPUT}
            DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
else()
    # install(FILES) copies symlinks verbatim, so install the real Cargo output
    # renamed to the versioned name, then recreate the soname/dev symlinks in the
    # install tree — the same layout CMake would generate for a versioned library.
    install(FILES ${CARGO_OUTPUT}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RENAME libservoshell.so.${SERVO_VERSION}
    )
    install(CODE "
        file(CREATE_LINK libservoshell.so.${SERVO_VERSION} \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libservoshell.so.${SERVO_SOVERSION}\" SYMBOLIC)
        file(CREATE_LINK libservoshell.so.${SERVO_SOVERSION}     \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libservoshell.so\"   SYMBOLIC)
    ")
endif()
