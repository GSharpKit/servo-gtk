//! C ABI for the context menu Servo asks the embedder to show.
//!
//! On a right click the page builds the menu itself: which entries apply
//! (Back, Reload, Copy, Copy Link, ...), which are enabled -- Copy only with a
//! selection, Paste only in editable text -- and what was under the pointer.
//! The embedder only has to present it and report the choice, so the menu
//! reaches C as one opaque [`ServoContextMenu`]. The host owns it until it
//! calls exactly one of [`servo_context_menu_select`] or
//! [`servo_context_menu_dismiss`], which may happen later -- a GTK menu is
//! asynchronous.
//!
//! Choosing an entry runs it inside Servo: Copy puts the selection on the
//! system clipboard through Servo's own clipboard support, Reload reloads, and
//! so on.

use std::ffi::{CString, c_char};
use std::ptr;

use servo::{ContextMenu, ContextMenuAction, ContextMenuElementInformationFlags, ContextMenuItem};

/// `SERVO_CONTEXT_MENU_*` element flags, part of the ABI; keep in sync with
/// `servo-webview.h`.
mod servo_context_menu_flag {
    pub const LINK: u32 = 1 << 0;
    pub const IMAGE: u32 = 1 << 1;
    pub const EDITABLE_TEXT: u32 = 1 << 2;
    pub const SELECTION: u32 = 1 << 3;
}

/// `SERVO_CONTEXT_MENU_ACTION_*` values, part of the ABI; keep in sync with
/// `servo-webview.h`.
mod servo_context_menu_action {
    pub const GO_BACK: u32 = 0;
    pub const GO_FORWARD: u32 = 1;
    pub const RELOAD: u32 = 2;
    pub const COPY_LINK: u32 = 3;
    pub const OPEN_LINK_IN_NEW_WEB_VIEW: u32 = 4;
    pub const COPY_IMAGE_LINK: u32 = 5;
    pub const OPEN_IMAGE_IN_NEW_VIEW: u32 = 6;
    pub const CUT: u32 = 7;
    pub const COPY: u32 = 8;
    pub const PASTE: u32 = 9;
    pub const SELECT_ALL: u32 = 10;
    /// Not an action: what [`super::servo_context_menu_item_action`] reports
    /// for a separator.
    pub const SEPARATOR: u32 = u32::MAX;
}

fn action_to_c(action: ContextMenuAction) -> u32 {
    use servo_context_menu_action::*;
    match action {
        ContextMenuAction::GoBack => GO_BACK,
        ContextMenuAction::GoForward => GO_FORWARD,
        ContextMenuAction::Reload => RELOAD,
        ContextMenuAction::CopyLink => COPY_LINK,
        ContextMenuAction::OpenLinkInNewWebView => OPEN_LINK_IN_NEW_WEB_VIEW,
        ContextMenuAction::CopyImageLink => COPY_IMAGE_LINK,
        ContextMenuAction::OpenImageInNewView => OPEN_IMAGE_IN_NEW_VIEW,
        ContextMenuAction::Cut => CUT,
        ContextMenuAction::Copy => COPY,
        ContextMenuAction::Paste => PASTE,
        ContextMenuAction::SelectAll => SELECT_ALL,
    }
}

fn action_from_c(action: u32) -> Option<ContextMenuAction> {
    use servo_context_menu_action::*;
    Some(match action {
        GO_BACK => ContextMenuAction::GoBack,
        GO_FORWARD => ContextMenuAction::GoForward,
        RELOAD => ContextMenuAction::Reload,
        COPY_LINK => ContextMenuAction::CopyLink,
        OPEN_LINK_IN_NEW_WEB_VIEW => ContextMenuAction::OpenLinkInNewWebView,
        COPY_IMAGE_LINK => ContextMenuAction::CopyImageLink,
        OPEN_IMAGE_IN_NEW_VIEW => ContextMenuAction::OpenImageInNewView,
        CUT => ContextMenuAction::Cut,
        COPY => ContextMenuAction::Copy,
        PASTE => ContextMenuAction::Paste,
        SELECT_ALL => ContextMenuAction::SelectAll,
        _ => return None,
    })
}

/// A pending context menu, owned by the host until it is selected from or
/// dismissed. The strings are converted once so the accessors can hand out
/// pointers that stay valid for the menu's lifetime.
pub struct ServoContextMenu {
    menu: ContextMenu,
    link_url: Option<CString>,
    image_url: Option<CString>,
    /// One per item; `None` for separators.
    labels: Vec<Option<CString>>,
}

impl ServoContextMenu {
    pub(crate) fn new(menu: ContextMenu) -> Self {
        let to_c = |s: &str| CString::new(s).ok();
        let info = menu.element_info();
        let link_url = info.link_url.as_ref().and_then(|url| to_c(url.as_str()));
        let image_url = info.image_url.as_ref().and_then(|url| to_c(url.as_str()));
        let labels = menu
            .items()
            .iter()
            .map(|item| match item {
                ContextMenuItem::Item { label, .. } => to_c(label),
                ContextMenuItem::Separator => None,
            })
            .collect();
        Self {
            menu,
            link_url,
            image_url,
            labels,
        }
    }

    fn item(&self, index: usize) -> Option<&ContextMenuItem> {
        self.menu.items().get(index)
    }
}

fn c_str_or_null(string: &Option<CString>) -> *const c_char {
    string.as_ref().map_or(ptr::null(), |s| s.as_ptr())
}

/// A bitmask of `SERVO_CONTEXT_MENU_*` flags describing what was under the
/// pointer: a link, an image, editable text, a selection.
///
/// # Safety
/// `menu` must be a live menu from the context-menu callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_context_menu_get_flags(menu: *const ServoContextMenu) -> u32 {
    let Some(menu) = (unsafe { menu.as_ref() }) else {
        return 0;
    };
    let flags = menu.menu.element_info().flags;
    let mut result = 0;
    for (servo_flag, c_flag) in [
        (ContextMenuElementInformationFlags::Link, servo_context_menu_flag::LINK),
        (ContextMenuElementInformationFlags::Image, servo_context_menu_flag::IMAGE),
        (
            ContextMenuElementInformationFlags::EditableText,
            servo_context_menu_flag::EDITABLE_TEXT,
        ),
        (
            ContextMenuElementInformationFlags::Selection,
            servo_context_menu_flag::SELECTION,
        ),
    ] {
        if flags.contains(servo_flag) {
            result |= c_flag;
        }
    }
    result
}

/// The link under the pointer, or NULL. Valid until the menu is finished.
///
/// # Safety
/// `menu` must be a live menu from the context-menu callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_context_menu_get_link_url(
    menu: *const ServoContextMenu,
) -> *const c_char {
    unsafe { menu.as_ref() }.map_or(ptr::null(), |menu| c_str_or_null(&menu.link_url))
}

/// The image under the pointer, or NULL. Valid until the menu is finished.
///
/// # Safety
/// `menu` must be a live menu from the context-menu callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_context_menu_get_image_url(
    menu: *const ServoContextMenu,
) -> *const c_char {
    unsafe { menu.as_ref() }.map_or(ptr::null(), |menu| c_str_or_null(&menu.image_url))
}

/// Number of items, separators included.
///
/// # Safety
/// `menu` must be a live menu from the context-menu callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_context_menu_get_n_items(menu: *const ServoContextMenu) -> usize {
    unsafe { menu.as_ref() }.map_or(0, |menu| menu.menu.items().len())
}

/// The `SERVO_CONTEXT_MENU_ACTION_*` of item `index`, or
/// `SERVO_CONTEXT_MENU_SEPARATOR` for a separator or an index out of range.
///
/// # Safety
/// `menu` must be a live menu from the context-menu callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_context_menu_item_action(
    menu: *const ServoContextMenu,
    index: usize,
) -> u32 {
    match unsafe { menu.as_ref() }.and_then(|menu| menu.item(index)) {
        Some(ContextMenuItem::Item { action, .. }) => action_to_c(*action),
        _ => servo_context_menu_action::SEPARATOR,
    }
}

/// The label Servo gives item `index` (English, e.g. "Copy"), or NULL for a
/// separator. Valid until the menu is finished.
///
/// # Safety
/// `menu` must be a live menu from the context-menu callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_context_menu_item_label(
    menu: *const ServoContextMenu,
    index: usize,
) -> *const c_char {
    unsafe { menu.as_ref() }
        .and_then(|menu| menu.labels.get(index))
        .map_or(ptr::null(), c_str_or_null)
}

/// Whether item `index` can be chosen right now -- e.g. Copy only with a
/// selection. False for separators.
///
/// # Safety
/// `menu` must be a live menu from the context-menu callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_context_menu_item_enabled(
    menu: *const ServoContextMenu,
    index: usize,
) -> bool {
    matches!(
        unsafe { menu.as_ref() }.and_then(|menu| menu.item(index)),
        Some(ContextMenuItem::Item { enabled: true, .. })
    )
}

/// Run `action` and free the menu. An unknown action dismisses instead.
///
/// # Safety
/// `menu` must be a live menu from the context-menu callback; it is freed
/// and must not be used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_context_menu_select(menu: *mut ServoContextMenu, action: u32) {
    if menu.is_null() {
        return;
    }
    let menu = unsafe { Box::from_raw(menu) };
    match action_from_c(action) {
        Some(action) => menu.menu.select(action),
        None => menu.menu.dismiss(),
    }
}

/// Close the menu without choosing anything, and free it.
///
/// # Safety
/// `menu` must be a live menu from the context-menu callback; it is freed
/// and must not be used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_context_menu_dismiss(menu: *mut ServoContextMenu) {
    if !menu.is_null() {
        unsafe { Box::from_raw(menu) }.menu.dismiss();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn actions_round_trip() {
        for action in 0..=servo_context_menu_action::SELECT_ALL {
            let servo_action = action_from_c(action).expect("every ABI value maps");
            assert_eq!(action_to_c(servo_action), action);
        }
    }

    #[test]
    fn unknown_actions_are_rejected() {
        assert!(action_from_c(servo_context_menu_action::SELECT_ALL + 1).is_none());
        assert!(action_from_c(servo_context_menu_action::SEPARATOR).is_none());
    }
}
