// Give the cdylib a proper soname so dependents (servogtk3, the demo) record
// NEEDED=libservo.so.0 rather than the raw Cargo output name. The file Cargo
// emits is still liblibservogtk.so (the crate can't be named `servo` — it would
// collide with the `servo` dependency it `use`s); the soname is what the dynamic
// linker and downstream NEEDED entries key off, so this is enough to present the
// library as "libservo" everywhere it matters. CMake then installs it under the
// versioned libservo.so.0.3.0 / libservo.so.0 / libservo.so names.
fn main() {
    println!("cargo::rustc-link-arg-cdylib=-Wl,-soname,libservo.so.0");
}
