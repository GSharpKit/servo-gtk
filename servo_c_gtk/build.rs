// Give the cdylib a stable name so dependents (servogtk3, the demo) record a
// dependency on "libservoshell" rather than the raw Cargo output name. The file
// Cargo emits is still libservoshell.<ext> (the crate can't be named `servo` —
// it would collide with the `servo` dependency it `use`s); what dependents key
// off is the object format's embedded library name, so stamping that is enough
// to present the library as "libservoshell" everywhere it matters. CMake then
// lays down the versioned libservoshell aliases next to it.
//
// Each object format spells "embedded library name" differently, so branch on
// the target OS (CARGO_CFG_TARGET_OS is set by Cargo for the build script):
//   * ELF   (Linux/BSD): -soname libservoshell.so.0
//   * Mach-O (macOS):    -install_name @rpath/libservoshell.0.dylib
//   * PE    (Windows):   no soname/install_name concept — nothing to stamp.
//
// The ELF and Mach-O names both carry the ABI major (0) and match the versioned
// alias CMake installs next to the real file, so dependents keep resolving the
// ABI they were linked against. Bump both when SERVO_SOVERSION changes.
fn main() {
    // Stamp the build time into the library so a running copy can say how old
    // it is. The whole point is telling "my fix did not work" apart from "I am
    // running yesterday's DLL", which is otherwise invisible on Windows, where
    // a stale libservoshell.dll next to the .exe silently wins over a fresh
    // one. Printed once at startup when SERVO_LOG_FILE or RUST_LOG is set.
    //
    // No `rerun-if-changed` is emitted anywhere in this script, so Cargo falls
    // back to re-running it whenever anything in the package changes -- which
    // is exactly when the stamp should move. A no-op build keeps the old
    // stamp, so an unchanged number across two builds is itself the answer:
    // nothing was rebuilt.
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or_default();
    println!("cargo::rustc-env=SERVO_BUILD_STAMP={stamp}");

    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    match target_os.as_str() {
        "macos" | "ios" => {
            println!(
                "cargo::rustc-link-arg-cdylib=-Wl,-install_name,@rpath/libservoshell.0.dylib"
            );
        }
        "windows" => {
            // DLLs are loaded by file name from the binary directory; there is
            // no soname/install_name to set.
        }
        _ => {
            println!("cargo::rustc-link-arg-cdylib=-Wl,-soname,libservoshell.so.0");
        }
    }
}
