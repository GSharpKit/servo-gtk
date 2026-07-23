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
    Code, Cursor, DeviceIntRect, DeviceVector2D, InputEvent, JSValue, JavaScriptEvaluationError,
    Key, KeyState, KeyboardEvent, Location, Modifiers, MouseButton, MouseButtonAction,
    MouseButtonEvent, MouseMoveEvent, NamedKey, PrefValue, Preferences, RenderingContext, Scroll,
    Servo, ServoBuilder, SoftwareRenderingContext, WebView, WebViewBuilder, WebViewDelegate,
    WebViewPoint, WebViewVector,
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

/// Called when the webview's URL changes — navigation, redirect, link
/// activation or history traversal. `url` is a NUL-terminated UTF-8 string,
/// valid only for the duration of the call; copy it before returning.
pub type ServoUrlChangedCallback =
    extern "C" fn(url: *const c_char, user_data: *mut c_void);

struct FrameCallback {
    func: ServoFrameReadyCallback,
    user_data: *mut c_void,
}

struct CursorCallback {
    func: ServoCursorChangedCallback,
    user_data: *mut c_void,
}

struct UrlCallback {
    func: ServoUrlChangedCallback,
    user_data: *mut c_void,
}

/// Servo delegate that turns presented frames, cursor changes and URL changes
/// into calls into the registered C callbacks.
struct EmbedderDelegate {
    rendering_context: Rc<dyn RenderingContext>,
    frame_callback: RefCell<Option<FrameCallback>>,
    cursor_callback: RefCell<Option<CursorCallback>>,
    url_callback: RefCell<Option<UrlCallback>>,
}

impl EmbedderDelegate {
    fn new(rendering_context: Rc<dyn RenderingContext>) -> Self {
        Self {
            rendering_context,
            frame_callback: RefCell::new(None),
            cursor_callback: RefCell::new(None),
            url_callback: RefCell::new(None),
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

    fn notify_url_changed(&self, _webview: WebView, url: Url) {
        let Some(cb) = self.url_callback.borrow().as_ref().map(|c| (c.func, c.user_data))
        else {
            return;
        };
        if let Ok(url) = CString::new(url.as_str()) {
            (cb.0)(url.as_ptr(), cb.1);
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

/// Experimental Web-platform features, gated behind preferences that default to
/// off. Enabling all of these is the equivalent of servoshell's
/// `--enable-experimental-web-platform-features` flag.
///
/// This mirrors the `// feature:`-annotated preferences in `servo-config`'s
/// `prefs.rs` (v0.3.0). Servo exposes no runtime "is experimental" query, so the
/// list is maintained by hand — revisit it when bumping the `servo` dependency.
/// Names are looked up with [`Preferences::exists`] before being set, so an entry
/// that disappears in a future version is skipped rather than panicking.
const EXPERIMENTAL_WEB_PLATFORM_FEATURES: &[&str] = &[
    "dom_webgpu_enabled",
    "dom_abort_controller_enabled",
    "dom_adoptedstylesheet_enabled",
    "dom_async_clipboard_enabled",
    "dom_canvas_capture_enabled",
    "dom_cookiestore_enabled",
    "dom_credential_management_enabled",
    "dom_crypto_subtle_enabled",
    "dom_exec_command_enabled",
    "dom_fontface_enabled",
    "dom_gamepad_enabled",
    "dom_geolocation_enabled",
    "dom_wakelock_enabled",
    "dom_indexeddb_enabled",
    "dom_intersection_observer_enabled",
    "dom_mutation_observer_enabled",
    "dom_navigator_protocol_handlers_enabled",
    "dom_notification_enabled",
    "dom_offscreen_canvas_enabled",
    "dom_permissions_enabled",
    "dom_resize_observer_enabled",
    "dom_sanitizer_enabled",
    "dom_storage_manager_api_enabled",
    "dom_serviceworker_enabled",
    "dom_sharedworker_enabled",
    "dom_webgl2_enabled",
    "dom_webrtc_enabled",
    "dom_webrtc_transceiver_enabled",
    "dom_webvtt_enabled",
    "dom_webxr_layers_enabled",
    "dom_visual_viewport_enabled",
    "largest_contentful_paint_enabled",
    "layout_columns_enabled",
    "layout_grid_enabled",
    "layout_variable_fonts_enabled",
    "layout_writing_mode_enabled",
];

/// Build the default [`Preferences`] with every experimental Web-platform
/// feature in [`EXPERIMENTAL_WEB_PLATFORM_FEATURES`] turned on.
fn experimental_preferences() -> Preferences {
    let mut preferences = Preferences::default();
    for name in EXPERIMENTAL_WEB_PLATFORM_FEATURES {
        if Preferences::exists(name) {
            preferences.set_value(name, PrefValue::Bool(true));
        }
    }
    preferences
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

    let servo = ServoBuilder::default()
        .preferences(experimental_preferences())
        .build();

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

/// Register the URL-change callback, invoked whenever the webview navigates to a
/// new URL. Pass a NULL `callback` to clear it.
///
/// # Safety
/// `webview` must be a valid handle. `user_data` is stored verbatim and handed
/// back to the callback; its lifetime is the caller's responsibility.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_set_url_changed_callback(
    webview: *mut ServoWebViewHandle,
    callback: Option<ServoUrlChangedCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };
    *handle.delegate.url_callback.borrow_mut() =
        callback.map(|func| UrlCallback { func, user_data });
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

/// Named-key identifiers understood by [`servo_webview_key`]. These mirror the
/// `ServoKey` enum in `servo-webview.h` — keep the two in sync. Values other
/// than `CHARACTER` map to a [`NamedKey`]; `CHARACTER` means "use the Unicode
/// codepoint instead".
mod servo_key {
    pub const CHARACTER: u32 = 0;
    pub const UNIDENTIFIED: u32 = 1;
    pub const ENTER: u32 = 2;
    pub const TAB: u32 = 3;
    pub const BACKSPACE: u32 = 4;
    pub const DELETE: u32 = 5;
    pub const ESCAPE: u32 = 6;
    pub const ARROW_LEFT: u32 = 7;
    pub const ARROW_RIGHT: u32 = 8;
    pub const ARROW_UP: u32 = 9;
    pub const ARROW_DOWN: u32 = 10;
    pub const HOME: u32 = 11;
    pub const END: u32 = 12;
    pub const PAGE_UP: u32 = 13;
    pub const PAGE_DOWN: u32 = 14;
}

/// Modifier bits understood by [`servo_webview_key`]. Mirrors the
/// `SERVO_MODIFIER_*` flags in `servo-webview.h`.
mod servo_modifier {
    pub const SHIFT: u32 = 1 << 0;
    pub const CONTROL: u32 = 1 << 1;
    pub const ALT: u32 = 1 << 2;
    pub const META: u32 = 1 << 3;
}

/// Report a key press (`pressed == true`) or release to the webview.
///
/// * `key` — a `ServoKey` value. Named keys (Enter, Tab, arrows, …) map to the
///   corresponding [`NamedKey`]; `SERVO_KEY_CHARACTER` (0) means the key
///   produced text, carried in `unicode`.
/// * `unicode` — the Unicode codepoint of the typed character when `key` is
///   `SERVO_KEY_CHARACTER`, otherwise ignored (pass 0).
/// * `modifiers` — a bitmask of `SERVO_MODIFIER_*` flags.
///
/// # Safety
/// `webview` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_key(
    webview: *mut ServoWebViewHandle,
    key: u32,
    unicode: u32,
    modifiers: u32,
    pressed: bool,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };

    let logical_key = match key {
        servo_key::ENTER => Key::Named(NamedKey::Enter),
        servo_key::TAB => Key::Named(NamedKey::Tab),
        servo_key::BACKSPACE => Key::Named(NamedKey::Backspace),
        servo_key::DELETE => Key::Named(NamedKey::Delete),
        servo_key::ESCAPE => Key::Named(NamedKey::Escape),
        servo_key::ARROW_LEFT => Key::Named(NamedKey::ArrowLeft),
        servo_key::ARROW_RIGHT => Key::Named(NamedKey::ArrowRight),
        servo_key::ARROW_UP => Key::Named(NamedKey::ArrowUp),
        servo_key::ARROW_DOWN => Key::Named(NamedKey::ArrowDown),
        servo_key::HOME => Key::Named(NamedKey::Home),
        servo_key::END => Key::Named(NamedKey::End),
        servo_key::PAGE_UP => Key::Named(NamedKey::PageUp),
        servo_key::PAGE_DOWN => Key::Named(NamedKey::PageDown),
        servo_key::UNIDENTIFIED => Key::Named(NamedKey::Unidentified),
        // CHARACTER (and any unknown value) falls back to the codepoint, then
        // to Unidentified if it isn't a valid scalar value.
        _ => match char::from_u32(unicode) {
            Some(c) if key == servo_key::CHARACTER => Key::Character(c.to_string()),
            _ => Key::Named(NamedKey::Unidentified),
        },
    };

    let mut mods = Modifiers::empty();
    if modifiers & servo_modifier::SHIFT != 0 {
        mods |= Modifiers::SHIFT;
    }
    if modifiers & servo_modifier::CONTROL != 0 {
        mods |= Modifiers::CONTROL;
    }
    if modifiers & servo_modifier::ALT != 0 {
        mods |= Modifiers::ALT;
    }
    if modifiers & servo_modifier::META != 0 {
        mods |= Modifiers::META;
    }

    let state = if pressed { KeyState::Down } else { KeyState::Up };

    handle
        .webview
        .notify_input_event(InputEvent::Keyboard(KeyboardEvent::new_without_event(
            state,
            logical_key,
            Code::Unidentified,
            Location::Standard,
            mods,
            false, // repeat
            false, // is_composing
        )));
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

/// Called once when an [`servo_webview_evaluate_script`] evaluation finishes.
///
/// Exactly one of `result_json` / `error` is non-NULL: on success `result_json`
/// is the return value serialized as a JSON string, on failure `error` is a
/// human-readable message. Both pointers are valid only for the duration of the
/// call — copy anything you need before returning.
pub type ServoScriptResultCallback = extern "C" fn(
    result_json: *const c_char,
    error: *const c_char,
    user_data: *mut c_void,
);

/// Convert Servo's [`JSValue`] into a plain [`serde_json::Value`].
///
/// `JSValue` derives serde `Serialize`, but that produces an externally-tagged
/// encoding (e.g. `{"Number":3.0}`); the C side wants the natural JSON shape
/// (`3`, `"foo"`, `[…]`), so map the variants explicitly. Node-reference
/// variants (Element, ShadowRoot, …) carry an opaque id string and are surfaced
/// as that string.
fn jsvalue_to_json(value: &JSValue) -> serde_json::Value {
    use serde_json::Value;
    match value {
        JSValue::Undefined | JSValue::Null => Value::Null,
        JSValue::Boolean(b) => Value::Bool(*b),
        JSValue::Number(n) => serde_json::Number::from_f64(*n)
            .map(Value::Number)
            .unwrap_or(Value::Null),
        JSValue::String(s)
        | JSValue::Element(s)
        | JSValue::ShadowRoot(s)
        | JSValue::Frame(s)
        | JSValue::Window(s) => Value::String(s.clone()),
        JSValue::Array(items) => Value::Array(items.iter().map(jsvalue_to_json).collect()),
        JSValue::Object(map) => Value::Object(
            map.iter()
                .map(|(k, v)| (k.clone(), jsvalue_to_json(v)))
                .collect(),
        ),
    }
}

/// A human-readable, one-line message for a [`JavaScriptEvaluationError`].
fn javascript_error_message(error: &JavaScriptEvaluationError) -> String {
    match error {
        JavaScriptEvaluationError::DocumentNotFound => {
            "the document that would run the script no longer exists".to_string()
        }
        JavaScriptEvaluationError::CompilationFailure => {
            "the script could not be compiled".to_string()
        }
        JavaScriptEvaluationError::EvaluationFailure(info) => match info {
            Some(info) => format!("uncaught exception: {}", info.message),
            None => "the script threw an uncaught exception".to_string(),
        },
        JavaScriptEvaluationError::InternalError => "internal Servo error".to_string(),
        JavaScriptEvaluationError::WebViewNotReady => "the web view is not ready".to_string(),
        JavaScriptEvaluationError::SerializationError(_) => {
            "the result could not be serialized".to_string()
        }
    }
}

/// Evaluate `script` as JavaScript in the webview's top-level browsing context.
///
/// Evaluation is asynchronous: `callback` is invoked exactly once, later, from
/// inside a [`servo_webview_spin`] call on the same thread. On success it
/// receives the return value serialized as a JSON string; on failure it
/// receives an error message. `user_data` is passed back verbatim. A NULL
/// `callback` is a no-op.
///
/// # Safety
/// `webview` must be a valid handle and `script` a valid NUL-terminated C
/// string. `user_data` is stored verbatim and handed back to the callback; the
/// caller owns its lifetime and must keep it valid until the callback fires.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_webview_evaluate_script(
    webview: *mut ServoWebViewHandle,
    script: *const c_char,
    callback: Option<ServoScriptResultCallback>,
    user_data: *mut c_void,
) {
    let Some(handle) = (unsafe { as_handle(webview) }) else {
        return;
    };

    let Some(callback) = callback else {
        return;
    };

    if script.is_null() {
        let error = CString::new("script is NULL").unwrap();
        callback(ptr::null(), error.as_ptr(), user_data);
        return;
    }

    let script = match unsafe { CStr::from_ptr(script) }.to_str() {
        Ok(script) => script,
        Err(_) => {
            let error = CString::new("script is not valid UTF-8").unwrap();
            callback(ptr::null(), error.as_ptr(), user_data);
            return;
        }
    };

    // `evaluate_javascript` converts the &str to an owned String synchronously
    // (before returning), so the borrow does not need to outlive this call. The
    // result closure is 'static and captures only the C function pointer and the
    // opaque user_data; it fires from a later spin_event_loop() on this thread.
    handle.webview.evaluate_javascript(script, move |result| {
        match result {
            Ok(value) => {
                let json = serde_json::to_string(&jsvalue_to_json(&value))
                    .unwrap_or_else(|_| "null".to_string());
                match CString::new(json) {
                    Ok(json) => callback(json.as_ptr(), ptr::null(), user_data),
                    Err(_) => {
                        // A NUL byte in the JSON (only possible inside a string
                        // value) can't be passed as a C string; report it.
                        let error =
                            CString::new("result contained an interior NUL byte").unwrap();
                        callback(ptr::null(), error.as_ptr(), user_data);
                    }
                }
            }
            Err(err) => {
                let error = CString::new(javascript_error_message(&err))
                    .unwrap_or_else(|_| CString::new("JavaScript evaluation failed").unwrap());
                callback(ptr::null(), error.as_ptr(), user_data);
            }
        }
    });
}
