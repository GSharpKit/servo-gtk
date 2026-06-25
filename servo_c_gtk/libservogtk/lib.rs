//! C ABI wrapper around Servo's [`servo::WebView`].
//!
//! This exposes a small, in-process C API so a GTK widget (or any other C
//! host) can create a Servo webview, drive its event loop, feed it pointer
//! input and receive rendered frames as tightly-packed RGBA8 buffers.
//!
//! # Threading
//!
//! Servo's `Servo`, `WebView` and `RenderingContext` are `Rc`-based and are
//! **not** `Send`/`Sync`. Every function in this module must be called from the
//! same thread that created the handle (the GTK main thread). Calling any of
//! them from another thread is undefined behaviour.
//!
//! # Lifecycle from the C side
//!
//! 1. `servo_webview_new()` — create a handle.
//! 2. `servo_webview_set_frame_ready_callback()` — register a frame sink.
//! 3. `servo_webview_load_uri()` / input / `servo_webview_resize()` as needed.
//! 4. `servo_webview_spin()` — call repeatedly from a GTK tick/timeout source;
//!    the frame-ready callback fires synchronously from inside this call.
//! 5. `servo_webview_free()` — destroy the handle.

use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char, c_void};
use std::ptr;
use std::rc::Rc;
use std::sync::Once;

use euclid::Point2D;
use servo::{
    Cursor, DeviceIntRect, DeviceVector2D, InputEvent, MouseButton, MouseButtonAction,
    MouseButtonEvent, MouseMoveEvent, RenderingContext, Scroll, Servo, ServoBuilder,
    SoftwareRenderingContext, WebView, WebViewBuilder, WebViewDelegate, WebViewPoint, WebViewVector,
};
use url::Url;

/// Called once per rendered frame with a tightly-packed RGBA8 buffer.
///
/// The pointer is only valid for the duration of the call; the host **must**
/// copy the pixels before returning. `width`/`height` are in device pixels and
/// the stride is always `width * 4` bytes.
pub type ServoFrameReadyCallback =
    extern "C" fn(rgba: *const u8, width: u32, height: u32, user_data: *mut c_void);

/// Called when the page requests a different cursor. `name` is a
/// NUL-terminated CSS cursor name (e.g. `"pointer"`), valid only for the
/// duration of the call.
pub type ServoCursorChangedCallback =
    extern "C" fn(name: *const c_char, user_data: *mut c_void);

struct FrameCallback {
    func: ServoFrameReadyCallback,
    user_data: *mut c_void,
}

struct CursorCallback {
    func: ServoCursorChangedCallback,
    user_data: *mut c_void,
}

/// Servo delegate that turns presented frames and cursor changes into calls
/// into the registered C callbacks.
struct EmbedderDelegate {
    rendering_context: Rc<dyn RenderingContext>,
    frame_callback: RefCell<Option<FrameCallback>>,
    cursor_callback: RefCell<Option<CursorCallback>>,
}

impl EmbedderDelegate {
    fn new(rendering_context: Rc<dyn RenderingContext>) -> Self {
        Self {
            rendering_context,
            frame_callback: RefCell::new(None),
            cursor_callback: RefCell::new(None),
        }
    }
}

impl WebViewDelegate for EmbedderDelegate {
    fn notify_new_frame_ready(&self, webview: WebView) {
        // Paint the frame, read it back, then present. `read_to_image` reads the
        // *back* buffer, which is what `paint()` just wrote; `present()` swaps it
        // out (with `PreserveBuffer::No`), so reading must happen *before*
        // presenting or we capture the stale, swapped-in buffer instead.
        let size = self.rendering_context.size2d().to_i32();
        let viewport_rect = DeviceIntRect::from_origin_and_size(Point2D::origin(), size);

        webview.paint();

        let image = self.rendering_context.read_to_image(viewport_rect);
        self.rendering_context.present();

        let Some(image) = image else {
            return;
        };

        let width = image.width();
        let height = image.height();
        let data = image.into_raw();

        // Copy the callback out before invoking it so the C side may safely
        // re-register a callback from within the call (no RefCell held).
        let cb = self
            .frame_callback
            .borrow()
            .as_ref()
            .map(|c| (c.func, c.user_data));
        if let Some((func, user_data)) = cb {
            func(data.as_ptr(), width, height, user_data);
        }
    }

    fn notify_cursor_changed(&self, _webview: WebView, cursor: Cursor) {
        let Some(cb) = self.cursor_callback.borrow().as_ref().map(|c| (c.func, c.user_data))
        else {
            return;
        };
        if let Ok(name) = CString::new(cursor_css_name(cursor)) {
            (cb.0)(name.as_ptr(), cb.1);
        }
    }
}

/// Opaque handle handed to C. Owns the whole Servo instance for one webview.
pub struct ServoWebViewHandle {
    servo: Servo,
    webview: WebView,
    delegate: Rc<EmbedderDelegate>,
    // Kept alive for as long as the webview lives; the delegate also holds a
    // type-erased clone of the same context.
    _rendering_context: Rc<SoftwareRenderingContext>,
}

static INIT: Once = Once::new();

/// One-time, process-global initialisation of the TLS crypto provider. Safe to
/// call repeatedly; only the first call has effect.
///
/// Servo's resource reader is intentionally *not* installed here. As of `servo`
/// 0.2.0 the reader is registered at compile time via `submit_resource_reader!`,
/// and the `servo-default-resources` crate (an unconditional dependency) bakes
/// one in — so a reader is always present and the old runtime `resources::set`
/// API no longer exists. There is no supported way to override it at runtime.
fn ensure_initialized() {
    INIT.call_once(|| {
        // Servo performs HTTPS itself and expects a default rustls crypto
        // provider to be installed by the embedder. Fail closed is not an
        // option here (it would abort the process), but a duplicate install is
        // harmless, so ignore the result.
        let _ = rustls::crypto::aws_lc_rs::default_provider().install_default();
    });
}

/// Map a Servo [`Cursor`] to its CSS cursor name.
fn cursor_css_name(cursor: Cursor) -> &'static str {
    match cursor {
        Cursor::Default => "default",
        Cursor::Pointer => "pointer",
        Cursor::Text => "text",
        Cursor::Wait => "wait",
        Cursor::Help => "help",
        Cursor::Crosshair => "crosshair",
        Cursor::Move => "move",
        Cursor::EResize => "e-resize",
        Cursor::NeResize => "ne-resize",
        Cursor::NwResize => "nw-resize",
        Cursor::NResize => "n-resize",
        Cursor::SeResize => "se-resize",
        Cursor::SwResize => "sw-resize",
        Cursor::SResize => "s-resize",
        Cursor::WResize => "w-resize",
        Cursor::EwResize => "ew-resize",
        Cursor::NsResize => "ns-resize",
        Cursor::NeswResize => "nesw-resize",
        Cursor::NwseResize => "nwse-resize",
        Cursor::ColResize => "col-resize",
        Cursor::RowResize => "row-resize",
        Cursor::AllScroll => "all-scroll",
        Cursor::ZoomIn => "zoom-in",
        Cursor::ZoomOut => "zoom-out",
        Cursor::Alias => "alias",
        Cursor::Cell => "cell",
        Cursor::Copy => "copy",
        Cursor::ContextMenu => "context-menu",
        Cursor::NoDrop => "no-drop",
        Cursor::NotAllowed => "not-allowed",
        Cursor::Grab => "grab",
        Cursor::Grabbing => "grabbing",
        Cursor::VerticalText => "vertical-text",
        Cursor::Progress => "progress",
        _ => "default",
    }
}

/// Borrow a handle pointer as a reference, returning early on NULL.
///
/// # Safety
/// `ptr` must be NULL or a pointer returned by [`servo_webview_new`] that has
/// not yet been passed to [`servo_webview_free`].
unsafe fn as_handle<'a>(ptr: *mut ServoWebViewHandle) -> Option<&'a ServoWebViewHandle> {
    unsafe { ptr.as_ref() }
}

/// Create a new Servo webview.
///
/// * `width` / `height` — initial surface size in device pixels.
/// * `initial_uri` — the URL to load when the webview is created, or NULL to
///   start on `about:blank`. Passing the initial URL here (rather than calling
///   [`servo_webview_load_uri`] right after creation) is required for it to
///   take effect: Servo creates the top-level browsing context together with
///   this URL, whereas a separate `load` issued before the browsing context
///   exists is dropped by the constellation. Invalid URLs fall back to
///   `about:blank`.
///
/// Returns an owning handle, or NULL on failure. Free it with
/// [`servo_webview_free`].
///
/// # Safety
/// `initial_uri` must be NULL or point to a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_new(
    width: u32,
    height: u32,
    initial_uri: *const c_char,
) -> *mut ServoWebViewHandle {
    ensure_initialized();

    let size = dpi::PhysicalSize::new(width.max(1), height.max(1));
    let rendering_context = match SoftwareRenderingContext::new(size) {
        Ok(context) => Rc::new(context),
        Err(_) => return ptr::null_mut(),
    };

    let initial_url = if initial_uri.is_null() {
        None
    } else {
        unsafe { CStr::from_ptr(initial_uri) }
            .to_str()
            .ok()
            .and_then(|s| Url::parse(s).ok())
    };

    let servo = ServoBuilder::default().build();

    let delegate = Rc::new(EmbedderDelegate::new(rendering_context.clone()));
    let mut builder = WebViewBuilder::new(&servo, rendering_context.clone())
        .delegate(delegate.clone());
    if let Some(url) = initial_url {
        builder = builder.url(url);
    }
    let webview = builder.build();
    webview.focus();
    webview.show();

    let handle = Box::new(ServoWebViewHandle {
        servo,
        webview,
        delegate,
        _rendering_context: rendering_context,
    });
    Box::into_raw(handle)
}

/// Destroy a webview handle. Passing NULL is a no-op.
///
/// # Safety
/// `webview` must be NULL or a pointer from [`servo_webview_new`] that has not
/// already been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_free(webview: *mut ServoWebViewHandle) {
    if webview.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(webview) });
}

/// Register the frame sink invoked once per presented frame. Pass a NULL
/// `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_frame_ready_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoFrameReadyCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.frame_callback.borrow_mut() =
        callback.map(|func| FrameCallback { func, user_data });
}

/// Register the cursor-change callback. Pass a NULL `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_cursor_changed_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoCursorChangedCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.cursor_callback.borrow_mut() =
        callback.map(|func| CursorCallback { func, user_data });
}

/// Begin loading `uri`. Invalid URLs are ignored.
///
/// # Safety
/// `webview` must be a valid handle and `uri` a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_load_uri(
    webview: *mut ServoWebViewHandle,
    uri: *const c_char,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    if uri.is_null() {
        return;
    }
    if let Ok(uri) = unsafe { CStr::from_ptr(uri) }.to_str() {
        if let Ok(url) = Url::parse(uri) {
            handle.webview.load(url);
        }
    }
}

/// Reload the current page.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_reload(webview: *mut ServoWebViewHandle) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle.webview.reload();
    }
}

/// Navigate one entry back in session history.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_go_back(webview: *mut ServoWebViewHandle) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        let _ = handle.webview.go_back(1);
    }
}

/// Navigate one entry forward in session history.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_go_forward(webview: *mut ServoWebViewHandle) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        let _ = handle.webview.go_forward(1);
    }
}

/// Resize the webview surface to `width` x `height` device pixels.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_resize(
    webview: *mut ServoWebViewHandle,
    width: u32,
    height: u32,
) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle
            .webview
            .resize(dpi::PhysicalSize::new(width.max(1), height.max(1)));
    }
}

/// Report pointer movement to `(x, y)` in device pixels.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_pointer_move(
    webview: *mut ServoWebViewHandle,
    x: f64,
    y: f64,
) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle
            .webview
            .notify_input_event(InputEvent::MouseMove(MouseMoveEvent::new(
                WebViewPoint::Device(Point2D::new(x as f32, y as f32)),
            )));
    }
}

/// Report a pointer button press (`pressed != 0`) or release. `button` follows
/// GDK numbering: 1 = left, 2 = middle, 3 = right.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_pointer_button(
    webview: *mut ServoWebViewHandle,
    button: u32,
    pressed: bool,
    x: f64,
    y: f64,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    let mouse_button = match button {
        2 => MouseButton::Middle,
        3 => MouseButton::Right,
        _ => MouseButton::Left,
    };
    let action = if pressed {
        MouseButtonAction::Down
    } else {
        MouseButtonAction::Up
    };
    handle
        .webview
        .notify_input_event(InputEvent::MouseButton(MouseButtonEvent::new(
            action,
            mouse_button,
            WebViewPoint::Device(Point2D::new(x as f32, y as f32)),
        )));
}

/// Scroll by `(dx, dy)` wheel deltas.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_scroll(
    webview: *mut ServoWebViewHandle,
    dx: f64,
    dy: f64,
) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        // The 20.0 multiplier matches Servo's winit_minimal example.
        handle.webview.notify_scroll_event(
            Scroll::Delta(WebViewVector::Device(DeviceVector2D::new(
                20.0 * dx as f32,
                20.0 * dy as f32,
            ))),
            WebViewPoint::Device(Point2D::new(10.0, 10.0)),
        );
    }
}

/// Pump Servo's event loop once. Call this regularly from a GTK tick/timeout
/// source on the main thread; the frame-ready callback fires from inside it.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_spin(webview: *mut ServoWebViewHandle) {
    if let Some(handle) = unsafe { as_handle(webview) } {
        handle.servo.spin_event_loop();
    }
}

/// Return the currently loaded URL as a newly-allocated UTF-8 C string, or NULL
/// if none. Free the result with [`servo_string_free`].
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_get_uri(
    webview: *mut ServoWebViewHandle,
) -> *mut c_char {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return ptr::null_mut();
    };
    match handle.webview.url() {
        Some(url) => match CString::new(url.as_str()) {
            Ok(cstring) => cstring.into_raw(),
            Err(_) => ptr::null_mut(),
        },
        None => ptr::null_mut(),
    }
}

/// Free a string previously returned by this library (e.g.
/// [`servo_webview_get_uri`]). Passing NULL is a no-op.
///
/// # Safety
/// `string` must be NULL or a pointer returned by this library and not yet
/// freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_string_free(string: *mut c_char) {
    if !string.is_null() {
        drop(unsafe { CString::from_raw(string) });
    }
}
