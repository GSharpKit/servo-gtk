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
//   * Mach-O (macOS):    -install_name @rpath/libservoshell.dylib
//   * PE    (Windows):   no soname/install_name concept — nothing to stamp.
fn main() {
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    match target_os.as_str() {
        "macos" | "ios" => {
            println!(
                "cargo::rustc-link-arg-cdylib=-Wl,-install_name,@rpath/libservoshell.dylib"
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
