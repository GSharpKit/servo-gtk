//! Making failures visible, especially on Windows.
//!
//! This library is a `cdylib` living inside someone else's process, and the
//! things most likely to go wrong -- a panic on one of Servo's own threads, an
//! `abort()` from the C++ rasterizer, a rendering context that cannot be built
//! -- all report through channels the embedder never sees:
//!
//! * A GUI-subsystem Windows process has no stderr at all, and even a console
//!   one loses it when started from Explorer. The app just closes.
//! * A panic crossing `extern "C"`, or one on a Servo worker thread, aborts
//!   the process before anything is flushed.
//! * Under .NET `P/Invoke` the host swallows native stderr entirely.
//!
//! So both routes here write to the file named by `SERVO_LOG_FILE`, which
//! survives all three. Without that variable set nothing is written and
//! nothing changes: a library must not litter a host's filesystem uninvited.

use std::ffi::OsStr;
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::panic;
use std::sync::Once;

/// Open the `SERVO_LOG_FILE` target for appending, creating it if needed.
///
/// Appends rather than truncates: a crash-and-retry loop is exactly when this
/// is being read, and each run should add to the story rather than erase the
/// previous one.
pub(crate) fn open_log(path: &OsStr) -> std::io::Result<File> {
    OpenOptions::new().create(true).append(true).open(path)
}

/// Write one line to the log file, if one was configured.
///
/// Deliberately opens the file per call instead of holding a handle. This is
/// reached from a panic hook, possibly while another thread is mid-write and
/// possibly while the process is coming down; there is no lock to poison and
/// no buffered state to lose. Every error is swallowed -- failing to write a
/// diagnostic must never itself become the failure.
fn append(line: &str) {
    let Some(path) = std::env::var_os("SERVO_LOG_FILE") else {
        return;
    };
    if let Ok(mut file) = open_log(&path) {
        let _ = file.write_all(line.as_bytes());
        let _ = file.write_all(b"\n");
        let _ = file.flush();
    }
}

/// Record a startup milestone to `SERVO_LOG_FILE`, unbuffered.
///
/// Deliberately not routed through `log`: this exists to survive a hard crash
/// -- an access violation or an `abort()` out of the C++ rasterizer, neither
/// of which unwinds, runs a panic hook, or flushes a logger. `append` opens,
/// writes and flushes per call, so whatever reached the file is the truth
/// about how far the process got. No-op unless SERVO_LOG_FILE is set.
pub(crate) fn trace(milestone: &str) {
    append(&format!("[trace] {milestone}"));
    // Second channel, in case the first one cannot write. If the log shows the
    // startup banner (which goes through `log`) but no `[trace]` lines at all,
    // then `append` is what failed rather than the process -- worth being able
    // to tell apart, since the whole point is trusting what the file says.
    log::debug!("{milestone}");
}

static PANIC_HOOK: Once = Once::new();

/// Record panics to `SERVO_LOG_FILE` in addition to whatever already happens.
///
/// Chains to the previous hook rather than replacing it, so Rust's own
/// stderr report (and any hook the host installed) still runs. Installed once
/// per process; calling it again is a no-op.
pub(crate) fn install_panic_hook() {
    PANIC_HOOK.call_once(|| {
        // Panics are only half the story on Windows; the other half never
        // reaches a Rust hook at all.
        #[cfg(windows)]
        windows_crash::install();
        install_abort_handler();

        let previous = panic::take_hook();
        panic::set_hook(Box::new(move |info| {
            let location = info
                .location()
                .map(|l| format!("{}:{}:{}", l.file(), l.line(), l.column()))
                .unwrap_or_else(|| "<unknown location>".to_string());

            // `payload_as_str` is not stable for `&PanicHookInfo`, so unwrap
            // the two payload types that `panic!` and `assert!` actually
            // produce and fall back to an opaque note for anything else.
            let payload = info.payload();
            let message = payload
                .downcast_ref::<&str>()
                .copied()
                .or_else(|| payload.downcast_ref::<String>().map(String::as_str))
                .unwrap_or("<non-string panic payload>");

            let thread = std::thread::current();
            let name = thread.name().unwrap_or("<unnamed>");

            append(&format!(
                "panic on thread '{name}' at {location}: {message}\n\
                 note: this aborts the host process. Set RUST_BACKTRACE=1 for a backtrace."
            ));

            previous(info);
        }));
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    /// One test, not several: both the `SERVO_LOG_FILE` variable and the
    /// process-wide panic hook are global state, so separate `#[test]` fns
    /// would race each other under the default thread pool.
    #[test]
    fn panics_and_messages_reach_the_log_file() {
        let path = std::env::temp_dir().join(format!(
            "servo-diagnostics-test-{}.log",
            std::process::id()
        ));
        let _ = std::fs::remove_file(&path);

        // Nothing is written until the embedder opts in by naming a file.
        unsafe { std::env::remove_var("SERVO_LOG_FILE") };
        append("must not be written");
        assert!(
            !path.exists(),
            "append() created a file with SERVO_LOG_FILE unset"
        );

        unsafe { std::env::set_var("SERVO_LOG_FILE", &path) };
        append("first line");
        append("second line");

        let written = std::fs::read_to_string(&path).expect("log file should exist");
        assert!(written.contains("first line"));
        assert!(
            written.contains("second line"),
            "append() must not truncate: {written:?}"
        );

        // The hook has to survive a real unwind and record where it came from.
        install_panic_hook();
        let previous_len = written.len();
        let result = std::panic::catch_unwind(|| panic!("deliberate test panic"));
        assert!(result.is_err());

        let after = std::fs::read_to_string(&path).expect("log file should exist");
        assert!(
            after.len() > previous_len,
            "panic hook wrote nothing to the log file"
        );
        assert!(
            after.contains("deliberate test panic"),
            "panic payload missing from log: {after:?}"
        );
        assert!(
            after.contains("servo-diagnostics.rs"),
            "panic location missing from log: {after:?}"
        );

        unsafe { std::env::remove_var("SERVO_LOG_FILE") };
        let _ = std::fs::remove_file(&path);
    }
}

/// Windows crash reporting.
///
/// A panic hook is not enough here. The failures that matter on Windows --
/// an access violation, a stack overflow, an `abort()` out of C++ -- are SEH
/// exceptions, not Rust panics: nothing unwinds, no hook runs, and a GUI
/// process with no console simply vanishes. `SetUnhandledExceptionFilter`
/// runs for exactly the exceptions nobody handled, so it does not interfere
/// with the ones SpiderMonkey raises and handles on purpose (it uses access
/// violations for GC barriers and wasm bounds checks, which is why a vectored
/// handler would be the wrong tool).
#[cfg(windows)]
pub(crate) mod windows_crash {
    use std::ffi::OsString;
    use std::os::windows::ffi::OsStringExt;
    use std::sync::atomic::{AtomicPtr, Ordering};

    use windows_sys::Win32::Foundation::{HMODULE, MAX_PATH};
    use windows_sys::Win32::System::Diagnostics::Debug::{
        EXCEPTION_POINTERS, SetUnhandledExceptionFilter,
    };
    use windows_sys::Win32::System::LibraryLoader::{
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        GetModuleFileNameW, GetModuleHandleExW,
    };

    /// Whatever filter was installed before ours, so the host's own crash
    /// reporting still runs after we have recorded the exception.
    static PREVIOUS: AtomicPtr<core::ffi::c_void> = AtomicPtr::new(std::ptr::null_mut());

    /// The exception codes worth naming. Everything else is printed as hex.
    fn describe(code: u32) -> &'static str {
        match code {
            0xC000_0005 => "ACCESS_VIOLATION",
            0xC000_001D => "ILLEGAL_INSTRUCTION",
            0xC000_0025 => "NONCONTINUABLE_EXCEPTION",
            0xC000_008C => "ARRAY_BOUNDS_EXCEEDED",
            0xC000_008E => "FLT_DIVIDE_BY_ZERO",
            0xC000_0094 => "INT_DIVIDE_BY_ZERO",
            0xC000_00FD => "STACK_OVERFLOW",
            0xC000_0409 => "STACK_BUFFER_OVERRUN / __fastfail",
            0xC000_0374 => "HEAP_CORRUPTION",
            0xE06D_7363 => "C++ exception",
            _ => "unknown",
        }
    }

    /// Name of the module containing `address`, so the log says *which*
    /// library faulted -- mozjs, swgl and Servo are all inside one DLL's
    /// address range, but a fault in a system DLL is worth telling apart.
    fn module_for(address: *const core::ffi::c_void) -> String {
        let mut module: HMODULE = std::ptr::null_mut();
        let ok = unsafe {
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                address.cast(),
                &mut module,
            )
        };
        if ok == 0 {
            return "<unknown module>".to_string();
        }
        let mut buffer = [0u16; MAX_PATH as usize];
        let len =
            unsafe { GetModuleFileNameW(module, buffer.as_mut_ptr(), buffer.len() as u32) };
        if len == 0 {
            return "<unnamed module>".to_string();
        }
        OsString::from_wide(&buffer[..len as usize])
            .to_string_lossy()
            .into_owned()
    }

    /// Records the exception, then hands off to whoever was there before.
    ///
    /// Kept deliberately small: for `STACK_OVERFLOW` this runs on the stack
    /// that just ran out, with only the guard page's worth of room left.
    unsafe extern "system" fn filter(info: *const EXCEPTION_POINTERS) -> i32 {
        if !info.is_null()
            && let Some(record) = unsafe { (*info).ExceptionRecord.as_ref() }
        {
            let code = record.ExceptionCode as u32;
            super::append(&format!(
                "FATAL exception 0x{code:08X} ({}) at {:p} in {}",
                describe(code),
                record.ExceptionAddress,
                module_for(record.ExceptionAddress),
            ));
            if code == 0xC000_00FD {
                super::append(
                    "note: STACK_OVERFLOW. Servo initialises on the calling thread, and \
                     Windows gives it far less stack than Linux does. Link the host with \
                     a larger stack reserve (the demos use -Wl,--stack,16777216).",
                );
            }
        }

        let previous = PREVIOUS.load(Ordering::SeqCst);
        if !previous.is_null() {
            type Filter = unsafe extern "system" fn(*const EXCEPTION_POINTERS) -> i32;
            let previous: Filter = unsafe { std::mem::transmute(previous) };
            return unsafe { previous(info) };
        }
        // EXCEPTION_CONTINUE_SEARCH: let the default handler produce whatever
        // dump or dialog the system is configured for. We only observe.
        0
    }

    /// Install the filter. Idempotent; safe to call more than once.
    pub(crate) fn install() {
        let previous = unsafe { SetUnhandledExceptionFilter(Some(filter)) };
        PREVIOUS.store(
            previous.map_or(std::ptr::null_mut(), |f| f as *mut core::ffi::c_void),
            Ordering::SeqCst,
        );
    }
}

static ABORT_HANDLER: Once = Once::new();

/// Record `abort()` before the process dies.
///
/// This is the one fatal exit neither other handler catches. It does not
/// unwind, so the panic hook never runs; and on Windows the CRT's `abort()`
/// terminates directly rather than raising a structured exception, so
/// `SetUnhandledExceptionFilter` never runs either. Both of the aborts this
/// codebase can actually hit end up here: swgl's `assert()` on an
/// unimplemented GL entry point, and Servo's `mozalloc_abort`.
///
/// The handler is not async-signal-safe, which is tolerable because this
/// `SIGABRT` is synthesised by a direct `abort()` call on the faulting
/// thread, not delivered asynchronously. It restores the default disposition
/// and re-raises so the process still dies the way it would have.
pub(crate) fn install_abort_handler() {
    extern "C" fn on_abort(_signal: i32) {
        append(
            "FATAL abort() called -- the process is terminating. No Rust panic and no \
             structured exception, so this is almost certainly an assert() in swgl (an \
             unimplemented GL entry point) or mozalloc_abort.",
        );
        unsafe {
            libc::signal(libc::SIGABRT, libc::SIG_DFL);
            libc::raise(libc::SIGABRT);
        }
    }

    ABORT_HANDLER.call_once(|| {
        unsafe {
            libc::signal(libc::SIGABRT, on_abort as *const () as libc::sighandler_t)
        };
    });
}
