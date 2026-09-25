//! A [`RenderingContext`] that rasterizes entirely on the CPU.
//!
//! # Why this exists
//!
//! Servo's compositor is WebRender, and WebRender only speaks OpenGL. The
//! embedding API has no non-GL path, so "run with no GL on the machine" cannot
//! mean "issue no GL calls" -- it has to mean "bring our own GL, in software".
//!
//! Servo's own [`SoftwareRenderingContext`] does *not* do that. It asks surfman
//! for a software *adapter*, which is Mesa llvmpipe on Unix and ANGLE over
//! Direct3D 11 WARP on Windows. Both still have to be installed and loadable,
//! which is exactly the deployment problem this wrapper is meant to avoid.
//!
//! [`swgl`] is WebRender's own rasterizer -- what Firefox ships as "Software
//! WebRender". It is C++ compiled into this cdylib and resolves no `libEGL`,
//! `libGLESv2`, `opengl32`, Direct3D or Mesa at all: no driver, no GPU, no
//! display server. WebRender recognizes it without being told, by comparing
//! `glGetString(GL_RENDERER)` against `"Software WebRender"`, and then turns
//! off optimized shaders and switches to its native clip-masking and
//! anti-aliasing paths.
//!
//! # What it costs
//!
//! WebGL and WebGPU canvases do not render: there is no GPU context to give
//! them. Everything else -- HTML, CSS, text, images, video -- rasterizes here.
//!
//! # Threading
//!
//! `swgl` keeps its current context in a thread-local, so every call has to
//! happen on the thread that called [`RenderingContext::make_current`] last.
//! That is the same constraint the rest of this crate already has: one GTK main
//! thread owns the webview (see the module docs in `servo-webview.rs`).
//!
//! [`SoftwareRenderingContext`]: servo::SoftwareRenderingContext

use std::cell::Cell;
use std::ptr;
use std::rc::Rc;
use std::sync::Arc;

use dpi::PhysicalSize;
use gleam::gl::{self, Gl};
use servo::{DeviceIntRect, RenderingContext, RgbaImage};
use surfman::Error;

/// A CPU-only [`RenderingContext`] backed by [`swgl`].
///
/// The rendered frame never reaches a window directly; it is read back with
/// [`RenderingContext::read_to_image`] and blitted by the GTK widget with
/// Cairo, which is what the embedder already did with the surfman context.
pub struct SwglRenderingContext {
    /// `swgl::Context` is a `Copy` handle around a C++ pointer. The `Rc` is the
    /// shape `gleam_gl_api` has to return; the bare copy is for our own calls.
    swgl: swgl::Context,
    gleam_gl: Rc<dyn Gl>,
    size: Cell<PhysicalSize<u32>>,
}

impl SwglRenderingContext {
    /// Create a context rendering into an `size`-sized CPU framebuffer that
    /// `swgl` allocates and owns.
    ///
    /// Note what is *not* here: no display connection, no adapter, no device.
    /// Stock Servo demands a `surfman::Connection` from every rendering
    /// context, and on Unix building one initialises an EGL display -- which
    /// drags in libEGL, the vendor GL driver, gbm, drm, gallium and LLVM on a
    /// machine that is supposed to need no GL at all. We do not provide one
    /// (see [`RenderingContext::connection`]'s default), and the patched
    /// `servo-paint` in `third_party/` accepts that.
    pub fn new(size: PhysicalSize<u32>) -> Result<Self, Error> {
        if size.width == 0 || size.height == 0 {
            log::error!("Unable to create SwglRenderingContext with size under 1x1 ({size:?})");
            return Err(Error::Failed);
        }

        // Milestones: this is C++ that can fault without unwinding, so the
        // only evidence of how far it got is what already reached the file.
        crate::diagnostics::trace("swgl: creating context");
        let swgl = swgl::Context::create();
        crate::diagnostics::trace("swgl: context created, making current");
        swgl.make_current();
        crate::diagnostics::trace(&format!(
            "swgl: current, allocating {}x{} framebuffer",
            size.width, size.height
        ));
        init_default_framebuffer(&swgl, size);
        crate::diagnostics::trace("swgl: framebuffer ready");

        Ok(Self {
            swgl,
            gleam_gl: Rc::new(swgl),
            size: Cell::new(size),
        })
    }
}

impl Drop for SwglRenderingContext {
    fn drop(&mut self) {
        self.swgl.destroy();
    }
}

impl RenderingContext for SwglRenderingContext {
    fn prepare_for_rendering(&self) {
        self.gleam_gl.bind_framebuffer(gl::FRAMEBUFFER, 0);
    }

    fn read_to_image(&self, source_rectangle: DeviceIntRect) -> Option<RgbaImage> {
        read_framebuffer_to_image(&self.gleam_gl, source_rectangle)
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
        init_default_framebuffer(&self.swgl, size);
    }

    /// No-op: `swgl` renders into a single CPU buffer, so there is nothing to
    /// swap. The embedder reads the finished frame out with `read_to_image`
    /// before calling this, and that read already sees what `paint()` wrote.
    fn present(&self) {}

    fn make_current(&self) -> Result<(), Error> {
        self.swgl.make_current();
        Ok(())
    }

    fn gleam_gl_api(&self) -> Rc<dyn Gl> {
        self.gleam_gl.clone()
    }

    /// Unsupported, and unreachable in practice: nothing in Servo calls this --
    /// its painter takes the `gleam` API above. `glow` builds its function
    /// table from a proc-address loader, and `swgl` is statically linked C++
    /// with no such loader to offer, so there is nothing honest to return.
    fn glow_gl_api(&self) -> Arc<glow::Context> {
        unimplemented!(
            "SwglRenderingContext renders on the CPU through the gleam API; \
             swgl exposes no proc-address loader for glow"
        )
    }

    // `connection()` is deliberately left at its default `None`. It exists so
    // Servo can open surfman devices for WebGL; there is no GPU context to
    // give WebGL here, and providing one would reintroduce the GL stack this
    // whole context exists to avoid.
}

/// (Re)point the default framebuffer at an `size`-sized buffer. A null `buf`
/// asks `swgl` to allocate and own the storage, and a zero `stride` lets it
/// pick a tightly-packed one.
fn init_default_framebuffer(swgl: &swgl::Context, size: PhysicalSize<u32>) {
    swgl.init_default_framebuffer(
        0,
        0,
        size.width as i32,
        size.height as i32,
        0,
        ptr::null_mut(),
    );
}

/// Read the default framebuffer back into an image.
///
/// A copy of `paint_api`'s `Framebuffer::read_framebuffer_to_image`, which is
/// private to that crate. Kept behaviourally identical so frames coming out of
/// this context are indistinguishable from the surfman context's, including
/// the vertical flip: GL's origin is bottom-left and `RgbaImage`'s is top-left.
fn read_framebuffer_to_image(gl: &Rc<dyn Gl>, source_rectangle: DeviceIntRect) -> Option<RgbaImage> {
    gl.bind_framebuffer(gl::FRAMEBUFFER, 0);
    gl.bind_vertex_array(0);

    let mut pixels = gl.read_pixels(
        source_rectangle.min.x,
        source_rectangle.min.y,
        source_rectangle.width(),
        source_rectangle.height(),
        gl::RGBA,
        gl::UNSIGNED_BYTE,
    );
    let gl_error = gl.get_error();
    if gl_error != gl::NO_ERROR {
        log::warn!("GL error code 0x{gl_error:x} set after read_pixels");
    }

    // flip image vertically (texture is upside down)
    let source_rectangle = source_rectangle.to_usize();
    let orig_pixels = pixels.clone();
    let stride = source_rectangle.width() * 4;
    for y in 0..source_rectangle.height() {
        let dst_start = y * stride;
        let src_start = (source_rectangle.height() - y - 1) * stride;
        let src_slice = &orig_pixels[src_start..src_start + stride];
        pixels[dst_start..dst_start + stride].clone_from_slice(&src_slice[..stride]);
    }

    RgbaImage::from_raw(
        source_rectangle.width() as u32,
        source_rectangle.height() as u32,
        pixels,
    )
}
