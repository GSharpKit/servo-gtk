//! A [`RenderingContext`] that renders on the GPU, inside a GL context the
//! host toolkit already created -- in practice a `GtkGLArea`'s.
//!
//! # Why this exists
//!
//! [`SwglRenderingContext`](crate::swgl_context::SwglRenderingContext) works
//! everywhere but rasterizes on the CPU and hands every frame over as an RGBA
//! buffer. When the toolkit *does* have GL, the frame can instead stay on the
//! GPU: WebRender draws into a framebuffer object owned by this context, and
//! the host's `render` handler blits it into its own framebuffer with
//! [`HostGlRenderingContext::blit_to_bound_framebuffer`]. No pixel crosses to
//! the CPU.
//!
//! The context is the host's, not one of ours, for two reasons. A separate
//! surfman context would not share objects with GTK's, so the frame could only
//! get across by reading it back -- the cost this exists to avoid. And GDK
//! caches which of *its* contexts is current: making a foreign context
//! current behind its back makes GDK skip its next real `MakeCurrent`. So the
//! context is only ever made current through the host's own callback.
//!
//! # Loading GL
//!
//! The host's GL library is already loaded by the time a context exists, so
//! the function table is resolved from whichever of EGL, GLX, WGL or CGL owns
//! the *current* context, looking only at libraries that are already in the
//! process (`RTLD_NOLOAD` / `GetModuleHandle`). Nothing here loads a GL
//! library on a machine that has none, so CPU-only machines are unaffected.
//!
//! # Threading
//!
//! Same as the rest of this crate: one thread, the GTK main thread.

use std::cell::Cell;
use std::ffi::{CStr, CString, c_char, c_void};
use std::rc::Rc;
use std::sync::Arc;

use dpi::PhysicalSize;
use gleam::gl::{self, GLuint, Gl};
use servo::{DeviceIntRect, RenderingContext, RgbaImage};
use surfman::Error;

/// Makes the host's GL context current on the calling thread. Returns whether
/// it succeeded; `false` means there is no context (e.g. the GL area has been
/// unrealized) and no GL may be issued.
pub type ServoGlMakeCurrentCallback = extern "C" fn(user_data: *mut c_void) -> bool;

/// The same minimum the surfman contexts in `servo-paint-api` request: desktop
/// GL 3.2 or GLES 3.0.
const MIN_GL_VERSION: (u32, u32) = (3, 2);
const MIN_GLES_VERSION: (u32, u32) = (3, 0);

/// A GPU [`RenderingContext`] that draws into an offscreen framebuffer inside
/// the host's GL context.
pub struct HostGlRenderingContext {
    make_current: ServoGlMakeCurrentCallback,
    user_data: *mut c_void,
    gleam_gl: Rc<dyn Gl>,
    glow_gl: Arc<glow::Context>,
    size: Cell<PhysicalSize<u32>>,
    framebuffer: GLuint,
    color: GLuint,
    depth_stencil: GLuint,
}

impl HostGlRenderingContext {
    /// Wrap the host context that `make_current` makes current.
    ///
    /// Fails -- and the caller should fall back to the CPU context -- when the
    /// context cannot be made current, no GL library owns it, it is older than
    /// GL 3.2 / GLES 3.0, or the offscreen framebuffer is incomplete.
    pub fn new(
        size: PhysicalSize<u32>,
        make_current: ServoGlMakeCurrentCallback,
        user_data: *mut c_void,
    ) -> Result<Self, String> {
        if size.width == 0 || size.height == 0 {
            return Err(format!("size under 1x1 ({size:?})"));
        }
        if !make_current(user_data) {
            return Err("the host GL context could not be made current".to_string());
        }

        let loader = sys::Loader::for_current_context()?;
        let load = |name: &str| loader.load(name);

        // GL_VERSION reads the same through either table; it decides which
        // one WebRender actually gets.
        let probe = unsafe { gl::GlFns::load_with(load) };
        let version = probe.get_string(gl::VERSION);
        let renderer = probe.get_string(gl::RENDERER);
        let (is_gles, major, minor) = parse_gl_version(&version)
            .ok_or_else(|| format!("unrecognised GL_VERSION {version:?}"))?;
        let minimum = if is_gles { MIN_GLES_VERSION } else { MIN_GL_VERSION };
        if (major, minor) < minimum {
            return Err(format!(
                "{version:?} is older than the {}.{} WebRender needs",
                minimum.0, minimum.1
            ));
        }

        let gleam_gl = if is_gles {
            unsafe { gl::GlesFns::load_with(load) }
        } else {
            probe
        };
        let glow_gl = Arc::new(unsafe { glow::Context::from_loader_function(load) });
        log::warn!(
            "host GL rendering via {}: {renderer} ({version})",
            loader.api
        );

        let framebuffer = gleam_gl.gen_framebuffers(1)[0];
        let renderbuffers = gleam_gl.gen_renderbuffers(2);
        let context = Self {
            make_current,
            user_data,
            gleam_gl,
            glow_gl,
            size: Cell::new(size),
            framebuffer,
            color: renderbuffers[0],
            depth_stencil: renderbuffers[1],
        };

        context.allocate_storage(size);
        let gl = &context.gleam_gl;
        gl.bind_framebuffer(gl::FRAMEBUFFER, framebuffer);
        gl.framebuffer_renderbuffer(
            gl::FRAMEBUFFER,
            gl::COLOR_ATTACHMENT0,
            gl::RENDERBUFFER,
            context.color,
        );
        gl.framebuffer_renderbuffer(
            gl::FRAMEBUFFER,
            gl::DEPTH_STENCIL_ATTACHMENT,
            gl::RENDERBUFFER,
            context.depth_stencil,
        );
        let status = gl.check_frame_buffer_status(gl::FRAMEBUFFER);
        if status != gl::FRAMEBUFFER_COMPLETE {
            // Drop deletes the objects created so far.
            return Err(format!("offscreen framebuffer incomplete (0x{status:x})"));
        }
        context.clear();

        Ok(context)
    }

    /// Copy the last painted frame into the framebuffer currently bound for
    /// drawing -- the GL area's, from inside its `render` handler -- scaling
    /// it to `width` x `height` (the area's size in device pixels).
    ///
    /// The host's context must be current, which it is inside `render`.
    pub fn blit_to_bound_framebuffer(&self, width: u32, height: u32) {
        let gl = &self.gleam_gl;
        let mut target = [0];
        unsafe { gl.get_integer_v(gl::DRAW_FRAMEBUFFER_BINDING, &mut target) };
        let target = target[0] as GLuint;

        let size = self.size.get();
        let (width, height) = (width.max(1), height.max(1));
        let filter = if (width, height) == (size.width, size.height) {
            gl::NEAREST
        } else {
            gl::LINEAR
        };

        // Both framebuffers keep GL's bottom-left origin, so no flip. Scissor
        // and colour mask both apply to blits, and WebRender leaves the
        // former enabled.
        gl.bind_framebuffer(gl::READ_FRAMEBUFFER, self.framebuffer);
        gl.bind_framebuffer(gl::DRAW_FRAMEBUFFER, target);
        gl.disable(gl::SCISSOR_TEST);
        gl.color_mask(true, true, true, true);
        gl.blit_framebuffer(
            0,
            0,
            size.width as i32,
            size.height as i32,
            0,
            0,
            width as i32,
            height as i32,
            gl::COLOR_BUFFER_BIT,
            filter,
        );
        gl.bind_framebuffer(gl::FRAMEBUFFER, target);
    }

    fn allocate_storage(&self, size: PhysicalSize<u32>) {
        let gl = &self.gleam_gl;
        let (width, height) = (size.width as i32, size.height as i32);
        gl.bind_renderbuffer(gl::RENDERBUFFER, self.color);
        gl.renderbuffer_storage(gl::RENDERBUFFER, gl::RGBA8, width, height);
        gl.bind_renderbuffer(gl::RENDERBUFFER, self.depth_stencil);
        gl.renderbuffer_storage(gl::RENDERBUFFER, gl::DEPTH24_STENCIL8, width, height);
        gl.bind_renderbuffer(gl::RENDERBUFFER, 0);
    }

    /// Start a fresh buffer white, so a blit before the first paint shows the
    /// same neutral background the CPU path draws.
    fn clear(&self) {
        let gl = &self.gleam_gl;
        gl.bind_framebuffer(gl::FRAMEBUFFER, self.framebuffer);
        gl.disable(gl::SCISSOR_TEST);
        gl.clear_color(1.0, 1.0, 1.0, 1.0);
        gl.clear(gl::COLOR_BUFFER_BIT);
    }
}

impl Drop for HostGlRenderingContext {
    fn drop(&mut self) {
        // If the host context is already gone, so are these objects.
        if (self.make_current)(self.user_data) {
            self.gleam_gl.delete_framebuffers(&[self.framebuffer]);
            self.gleam_gl
                .delete_renderbuffers(&[self.color, self.depth_stencil]);
        }
    }
}

impl RenderingContext for HostGlRenderingContext {
    fn prepare_for_rendering(&self) {
        self.gleam_gl
            .bind_framebuffer(gl::FRAMEBUFFER, self.framebuffer);
    }

    fn read_to_image(&self, source_rectangle: DeviceIntRect) -> Option<RgbaImage> {
        if !(self.make_current)(self.user_data) {
            return None;
        }
        crate::swgl_context::read_framebuffer_to_image(
            &self.gleam_gl,
            self.framebuffer,
            source_rectangle,
        )
    }

    fn size(&self) -> PhysicalSize<u32> {
        self.size.get()
    }

    fn resize(&self, size: PhysicalSize<u32>) {
        assert!(
            size.width > 0 && size.height > 0,
            "Dimensions must be at least 1x1, got {size:?}",
        );

        if self.size.get() == size {
            return;
        }

        self.size.set(size);
        if (self.make_current)(self.user_data) {
            self.allocate_storage(size);
            self.clear();
        }
    }

    /// No-op: the frame stays in the offscreen framebuffer until the host
    /// blits it, and the next paint overwrites it completely.
    fn present(&self) {}

    fn make_current(&self) -> Result<(), Error> {
        if (self.make_current)(self.user_data) {
            Ok(())
        } else {
            Err(Error::Failed)
        }
    }

    fn gleam_gl_api(&self) -> Rc<dyn Gl> {
        self.gleam_gl.clone()
    }

    fn glow_gl_api(&self) -> Arc<glow::Context> {
        self.glow_gl.clone()
    }

    // `connection()` stays `None`: there is no surfman device behind the
    // host's context, so WebGL remains off here as on the CPU path.
}

/// Parse a `GL_VERSION` string into `(is_gles, major, minor)`.
///
/// Desktop GL reports `"<major>.<minor>[.<release>] <vendor info>"`; GLES
/// prefixes `"OpenGL ES "` (or `"OpenGL ES-CM "` / `"OpenGL ES-CL "` for the
/// 1.x profiles).
fn parse_gl_version(version: &str) -> Option<(bool, u32, u32)> {
    let version = version.trim_start();
    let (is_gles, rest) = match version.strip_prefix("OpenGL ES") {
        Some(rest) => (
            true,
            rest.strip_prefix("-CM")
                .or_else(|| rest.strip_prefix("-CL"))
                .unwrap_or(rest),
        ),
        None => (false, version),
    };

    let number = rest.trim_start().split_whitespace().next()?;
    let mut parts = number.split('.');
    let major = parts.next()?.parse().ok()?;
    let minor = parts
        .next()?
        .chars()
        .take_while(char::is_ascii_digit)
        .collect::<String>()
        .parse()
        .ok()?;
    Some((is_gles, major, minor))
}

/// Resolving GL entry points for whichever windowing API owns the current
/// context. Only libraries that are already loaded are ever looked at.
mod sys {
    use super::*;

    /// `eglGetProcAddress`, `glXGetProcAddressARB`, `wglGetProcAddress`.
    /// `EGLAPIENTRY` and `WINAPI` are both `__stdcall`, i.e. `"system"`; on
    /// every other target `"system"` is plain `"C"`.
    type GetProcAddress = unsafe extern "system" fn(*const c_char) -> *const c_void;
    /// `eglGetCurrentContext`, `glXGetCurrentContext`, `wglGetCurrentContext`,
    /// `CGLGetCurrentContext`.
    type GetCurrentContext = unsafe extern "system" fn() -> *const c_void;

    pub struct Loader {
        pub api: &'static str,
        get_proc_address: Option<GetProcAddress>,
        /// Searched with `dlsym` / `GetProcAddress` when `get_proc_address`
        /// has nothing: core GL 1.x on WGL, everything on CGL.
        libraries: Vec<Library>,
    }

    impl Loader {
        pub fn load(&self, name: &str) -> *const c_void {
            let Ok(name) = CString::new(name) else {
                return std::ptr::null();
            };
            if let Some(get_proc_address) = self.get_proc_address {
                let address = unsafe { get_proc_address(name.as_ptr()) };
                // wglGetProcAddress signals failure with small sentinels, not
                // only NULL.
                if !matches!(address as isize, -1..=3) {
                    return address;
                }
            }
            self.libraries
                .iter()
                .map(|library| library.symbol(&name))
                .find(|address| !address.is_null())
                .unwrap_or(std::ptr::null())
        }

        /// The first API in `candidates` whose library is loaded and reports
        /// a current context.
        fn first_current(candidates: &[Candidate]) -> Result<Self, String> {
            for candidate in candidates {
                let Some(library) = Library::loaded(candidate.libraries) else {
                    continue;
                };
                let get_current = library.symbol(candidate.get_current_context);
                if get_current.is_null() {
                    continue;
                }
                let get_current: GetCurrentContext = unsafe { std::mem::transmute(get_current) };
                if unsafe { get_current() }.is_null() {
                    continue;
                }

                let get_proc_address = candidate
                    .get_proc_address
                    .map(|name| library.symbol(name))
                    .filter(|address| !address.is_null())
                    .map(|address| unsafe {
                        std::mem::transmute::<*const c_void, GetProcAddress>(address)
                    });
                let mut libraries = vec![library];
                libraries.extend(
                    candidate
                        .fallback_libraries
                        .iter()
                        .filter_map(|name| Library::loaded(&[*name])),
                );
                return Ok(Loader {
                    api: candidate.api,
                    get_proc_address,
                    libraries,
                });
            }
            Err("no current EGL, GLX, WGL or CGL context in this process".to_string())
        }

        #[cfg(all(unix, not(target_vendor = "apple")))]
        pub fn for_current_context() -> Result<Self, String> {
            // EGL first: GTK4 uses it on X11 as well as Wayland, and an X11
            // process may have libGLX loaded without a GLX context current.
            Self::first_current(&[
                Candidate {
                    api: "EGL",
                    libraries: &[c"libEGL.so.1", c"libEGL.so"],
                    get_current_context: c"eglGetCurrentContext",
                    get_proc_address: Some(c"eglGetProcAddress"),
                    fallback_libraries: &[c"libGLESv2.so.2", c"libOpenGL.so.0", c"libGL.so.1"],
                },
                Candidate {
                    api: "GLX",
                    libraries: &[c"libGLX.so.0", c"libGL.so.1", c"libGL.so"],
                    get_current_context: c"glXGetCurrentContext",
                    get_proc_address: Some(c"glXGetProcAddressARB"),
                    fallback_libraries: &[c"libOpenGL.so.0", c"libGL.so.1"],
                },
            ])
        }

        #[cfg(target_vendor = "apple")]
        pub fn for_current_context() -> Result<Self, String> {
            Self::first_current(&[Candidate {
                api: "CGL",
                libraries: &[c"/System/Library/Frameworks/OpenGL.framework/OpenGL"],
                get_current_context: c"CGLGetCurrentContext",
                get_proc_address: None,
                fallback_libraries: &[],
            }])
        }

        #[cfg(windows)]
        pub fn for_current_context() -> Result<Self, String> {
            // GTK4 can run on ANGLE's EGL; otherwise it is plain WGL.
            Self::first_current(&[
                Candidate {
                    api: "EGL",
                    libraries: &[c"libEGL.dll"],
                    get_current_context: c"eglGetCurrentContext",
                    get_proc_address: Some(c"eglGetProcAddress"),
                    fallback_libraries: &[c"libGLESv2.dll"],
                },
                Candidate {
                    api: "WGL",
                    libraries: &[c"opengl32.dll"],
                    get_current_context: c"wglGetCurrentContext",
                    get_proc_address: Some(c"wglGetProcAddress"),
                    fallback_libraries: &[],
                },
            ])
        }
    }

    /// One windowing API to probe for a current context.
    struct Candidate {
        api: &'static str,
        /// Names it may be loaded under; the first one already loaded wins.
        libraries: &'static [&'static CStr],
        get_current_context: &'static CStr,
        get_proc_address: Option<&'static CStr>,
        /// Also searched by name, if already loaded.
        fallback_libraries: &'static [&'static CStr],
    }

    /// A shared library that was already loaded into the process. Never
    /// unloaded: the host keeps it loaded for as long as its context lives.
    struct Library(*mut c_void);

    #[cfg(unix)]
    impl Library {
        fn loaded(names: &[&CStr]) -> Option<Self> {
            names.iter().find_map(|name| {
                let handle =
                    unsafe { libc::dlopen(name.as_ptr(), libc::RTLD_LAZY | libc::RTLD_NOLOAD) };
                (!handle.is_null()).then_some(Library(handle))
            })
        }

        fn symbol(&self, name: &CStr) -> *const c_void {
            unsafe { libc::dlsym(self.0, name.as_ptr()) }
        }
    }

    #[cfg(windows)]
    impl Library {
        fn loaded(names: &[&CStr]) -> Option<Self> {
            use windows_sys::Win32::System::LibraryLoader::GetModuleHandleA;
            names.iter().find_map(|name| {
                let handle = unsafe { GetModuleHandleA(name.as_ptr().cast()) };
                (!handle.is_null()).then_some(Library(handle))
            })
        }

        fn symbol(&self, name: &CStr) -> *const c_void {
            use windows_sys::Win32::System::LibraryLoader::GetProcAddress;
            match unsafe { GetProcAddress(self.0, name.as_ptr().cast()) } {
                Some(address) => address as *const c_void,
                None => std::ptr::null(),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::parse_gl_version;

    #[test]
    fn desktop_versions() {
        assert_eq!(parse_gl_version("4.6 (Core Profile) Mesa 25.1.4"), Some((false, 4, 6)));
        assert_eq!(parse_gl_version("3.2.0 NVIDIA 570.86"), Some((false, 3, 2)));
        assert_eq!(parse_gl_version("4.1 Metal - 89.3"), Some((false, 4, 1)));
        assert_eq!(parse_gl_version("1.1.0"), Some((false, 1, 1)));
    }

    #[test]
    fn gles_versions() {
        assert_eq!(parse_gl_version("OpenGL ES 3.2 Mesa 25.1.4"), Some((true, 3, 2)));
        assert_eq!(
            parse_gl_version("OpenGL ES 3.0.0 (ANGLE 2.1.24081 git hash: 1b3e5c)"),
            Some((true, 3, 0))
        );
        assert_eq!(parse_gl_version("OpenGL ES-CM 1.1 Mesa"), Some((true, 1, 1)));
    }

    #[test]
    fn rejects_garbage() {
        assert_eq!(parse_gl_version(""), None);
        assert_eq!(parse_gl_version("OpenGL ES"), None);
        assert_eq!(parse_gl_version("unknown"), None);
        assert_eq!(parse_gl_version("4"), None);
    }
}
