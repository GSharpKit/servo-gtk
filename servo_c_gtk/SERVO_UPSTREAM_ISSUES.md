# Upstream reports for servo/servo

Five issues found while embedding Servo as an in-process webview and pointing it
at the stock PDF.js viewer. Together they make PDF.js 6.x unusable, but each is
independent and each has a minimal reproduction that does not involve PDF.js.

File separately — they touch script, layout/CSP, paint and DX respectively.

Issue 6 is unrelated to PDF.js: it is a one-line default in `servo-paint` that
makes Servo impossible to drive with a pure-CPU (swgl) rasterizer.

## Environment

| | |
|---|---|
| servo | 0.5.0 (crates.io), embedded in-process via a C ABI wrapper |
| mozjs_sys | 140.14.0-0 (SpiderMonkey 140) |
| stylo | 0.20.0 |
| rendering | `SoftwareRenderingContext`, 1200x900, driven by `spin_event_loop()` |
| rustc | 1.99.0-nightly (0e29c21d9 2026-07-21) |
| OS | Fedora Linux 44, kernel 7.1.12-200.fc44.x86_64 |
| build profile | **debug** — see the note under issue 2 |

Reproductions were served over plain HTTP from `python3 -m http.server`, loaded
with `WebViewBuilder`, and inspected with `WebView::evaluate_javascript` and the
RGBA readback from `RenderingContext::read_to_image`.

---

## 1. `Map.prototype.getOrInsert` / `getOrInsertComputed` are absent (`NIGHTLY_BUILD`-gated in mozjs)

**Observed**

```js
typeof Map.prototype.getOrInsert           // "undefined"
typeof Map.prototype.getOrInsertComputed   // "undefined"
typeof WeakMap.prototype.getOrInsertComputed // "undefined"
```

**Cause** — the methods exist in the bundled SpiderMonkey but are compiled out.
`mozjs_sys-140.14.0-0/mozjs/js/src/builtin/MapObject.cpp:391`:

```cpp
#ifdef NIGHTLY_BUILD
    JS_FN("getOrInsert", getOrInsert, 2, 0),
    JS_SELF_HOSTED_FN("getOrInsertComputed", "MapGetOrInsertComputed", 2, 0),
#endif
```

So this is a build-configuration gap rather than a version gap.

**Impact** — PDF.js 6.x uses `getOrInsertComputed` in its `EventBus.on`. The
stock viewer therefore throws during startup:

```
TypeError: this[#listeners].getOrInsertComputed is not a function
  EventBus.on (viewer.mjs:1343)
  → PDFFindController (viewer.mjs:6702)
  → _initializeViewerComponents (viewer.mjs:19070)
```

`PDFViewerApplication.initialize()` rejects, `run()` never reaches `open()`, and
the viewer renders its toolbar and a permanently empty document area.
`PDFViewerApplication.initialized === false`. Both the normal and `-legacy`
PDF.js distributions use it, so the legacy build is not a workaround.

Nothing is printed to the console — see issue 5, which is what made this take so
long to find.

**Suggested fix** — enable these in the mozjs build, or track SpiderMonkey
stabilising them. A six-line polyfill restores the stock viewer completely.

---

## 2. One `<input type="range">` plus any CSP that restricts inline styles collapses script throughput

**Reproduction** — `range.html`:

```html
<!DOCTYPE html><html><head><title>r</title>
<meta http-equiv="Content-Security-Policy" content="default-src 'none'">
</head><body>
<input type="range">
</body></html>
```

Load it, wait 4 s, then repeatedly `evaluate_javascript("1+1")` for 20 s and
count completions.

**Measured** (one range input, 20 s window)

| page | trivial evaluations completed |
|---|---|
| no CSP | **9303** |
| CSP `default-src 'none'` | **2** |

A ~4600x collapse. Removing the range input restores full speed; so does adding
`'unsafe-inline'` to `style-src`:

| ranges | no CSP | CSP `default-src 'none'` |
|---|---|---|
| 0 | 0.01 s | 0.01 s |
| 1 | 0.01 s | 3.11 s |
| 2 | 0.01 s | 7.46 s |
| 5 | 0.01 s | 4.64 s |
| 20 | 0.01 s | 4.62 s |

(worst of two `1+1` evaluations; the cost is not proportional to the number of
inputs, which suggests a repeating cost rather than a per-element one)

Other control types are unaffected — `checkbox`, `radio`, `number`, `text`,
`file`, `password` and `color` were all instant under the same CSP. Element
count is not the trigger either: 1600 `<div>`s under the same CSP are instant.

**Hypothesis** (not confirmed) — a CSP inline-style check being re-run against
the range input's UA shadow content on every restyle. The correlation with
`style-src 'unsafe-inline'` is what points that way.

**Impact** — PDF.js's viewer has five range inputs and ships its own
`<meta>` CSP with `style-src 'self'`. The result is a viewer that paints its
chrome and then takes minutes to render a page, or never does: 3 frames in 45 s,
versus 629 frames in 45 s once `'unsafe-inline'` is added.

**Caveat** — measured on a **debug** build of Servo, where everything is far
slower than release. The ratios should hold but the absolute numbers will not.

---

## 3. `clip-path: url(#id)` is accepted but never applied

**Reproduction** — `clip.html`:

```html
<!DOCTYPE html><html><head><style>
 div    { width:200px; height:200px; background:#c00; margin:4px }
 #shape { clip-path: inset(0 0 50% 0); }
 #ref   { clip-path: url(#half); }
</style></head><body>
<svg width="0" height="0">
  <clipPath id="half" clipPathUnits="objectBoundingBox">
    <rect x="0" y="0" width="1" height="0.5"/>
  </clipPath>
</svg>
<div id="plain"></div><div id="shape"></div><div id="ref"></div>
</body></html>
```

**Measured** by sampling the rendered frame:

| element | top half painted | bottom half painted | result |
|---|---|---|---|
| `#plain` (control) | yes | yes | not clipped, as expected |
| `#shape` — `inset(0 0 50% 0)` | yes | **no** | **clipped correctly** |
| `#ref` — `url(#half)` | yes | yes | **not clipped** |

Both report support:

```js
CSS.supports('clip-path', 'inset(0 0 50% 0)')  // true
CSS.supports('clip-path', 'url(#half)')        // true
```

So basic shapes work and SVG `<clipPath>` references are silently ignored, while
feature detection claims both. `clip-path` carries no `servo_pref` gate in
`stylo-0.20.0/properties/longhands.toml`, unlike `mask-image` (issue 4).

The clip is missing for **hit-testing** as well as painting, which is the more
damaging half.

**Impact** — PDF.js 6.x auto-detects URLs in page text and, because one detected
link can span several text runs, emits a single deliberately oversized
`<section class="linkAnnotation">` clipped back to the real runs with
`clip-path: url(#…)`. Measured on a real invoice: the annotation is 1178x3299
against a 1178x1658 page (`height: 201.151%`). Unclipped, PDF.js's
`.linkAnnotation > a:hover { opacity:.2; background:#ff0 }` tints the **whole
page** yellow on hover, and `elementFromPoint()` on blank paper hits the
anchor — so a click anywhere on the page follows the detected URL.

---

## 4. `mask-image` is unimplemented, which breaks icon-font-free UIs

Marked as such in `stylo-0.20.0/properties/longhands.toml`:

```toml
[mask-image]
servo_pref = "layout.unimplemented"
```

and `layout_unimplemented` defaults to `false` in `servo-config-0.5.0`.

**Observed**

```js
CSS.supports('mask-image', 'url(a.svg)')          // false
CSS.supports('-webkit-mask-image', 'url(a.svg)')  // false
CSS.supports('mask-size', 'cover')                // false
getComputedStyle(el).maskImage                    // ""
```

The declaration is dropped at parse time, so the masked SVGs are never even
fetched — 0 of 81 icon requests reached the server.

**Reproduction** — a 64x64 black box masked to a circle renders as the full
square; the mask has no effect.

**Impact** — this is the modern way to ship monochrome, themeable icons, and
PDF.js uses it for all ~90 of its toolbar icons:

```css
#zoomInButton::before {
  content: ""; width: 16px; height: 16px;
  background-color: var(--toolbar-icon-bg-color);
  mask-image: var(--toolbarButton-zoomIn-icon);
}
```

Every icon in the viewer — zoom, search, sidebar, print, download — renders as a
plain 16x16 black block. Reported here mainly to record the concrete
consequence; combined with issue 3 it makes masked-icon UIs look broken rather
than degraded.

---

## 5. Unhandled promise rejections are never reported to the embedder

**Reproduction**

```html
<script>
console.log("console works");
Promise.reject(new Error("unhandled rejection boom"));
(async function(){ throw new Error("async throw boom"); })();
setTimeout(function(){ console.log("still alive"); }, 500);
</script>
```

**Observed** with a `WebViewDelegate::show_console_message` implementation
attached:

```
[console] console works
[console] still alive
```

Both the rejection and the async throw vanish. Execution continues, so nothing
signals that anything went wrong.

**Impact** — this is what made issue 1 so hard to diagnose. A stock, widely
deployed application failed at startup with no diagnostic whatsoever: correct
page title, correct localisation, fully painted chrome, and an empty content
area. Finding it required calling the failing function by hand from
`evaluate_javascript` and attaching a `.catch()`.

Browsers surface these as console errors, and a page-side
`window.addEventListener("unhandledrejection", …)` is not a substitute for
embedders, since it requires modifying the page.

---

## 6. `servo-paint` hard-codes `clear_caches_with_quads`, which rules out software WebRender (swgl)

**Observed** — driving Servo with a `RenderingContext` whose `gleam_gl_api()` is
Mozilla's own software rasterizer (`swgl` 0.70, i.e. what Firefox ships as
"Software WebRender") aborts on the first composite:

```
gl.cc:1390: void DepthFunc(GLenum): Assertion `false' failed.
```

With `NDEBUG` defined (how Gecko ships swgl) there is no abort, but nothing
renders: every picture-cache tile stays as it was, because the clear is
silently dropped.

**Cause** — `servo-paint-0.5.0/painter.rs:226` builds `WebRenderOptions` with
`..Default::default()`, and WebRender defaults `clear_caches_with_quads` to
`true` (`webrender-0.70.0/src/renderer/init.rs:267`). That selects the
quad-based picture-cache clear in
`webrender-0.70.0/src/renderer/mod.rs:2857`:

```rust
Some(r) if self.clear_caches_with_quads => {
    self.device.enable_depth(DepthFunction::Always);
```

`ps_clear.glsl` draws that quad at the far plane (`gl_Position.z =
gl_Position.w; // force depth clear to 1.0`), so it only does its job under
`GL_ALWAYS`. swgl implements `GL_LESS` and `GL_LEQUAL` only
(`swgl-0.70.0/src/gl.cc:1384`), and treats anything else as `GL_LESS` — under
which a fragment at the maximum depth can never pass, so the tile is never
cleared.

The guard cannot be dodged from outside: `prefers_clear_scissor` is
unconditionally true off Android (`device/gl.rs:1943`), and
`supports_render_target_partial_update` keys off a `renderer_name` that must
simultaneously start with `"Software WebRender"` for `is_software_webrender`
(`device/gl.rs:1751`) and with `"Mali-T"`/`"PowerVR D-Series"` to be disabled
(`device/gl.rs:1873`).

**Fix** — one line, and it is what Gecko already does for Software WebRender:

```rust
clear_caches_with_quads: false,   // when the GL is swgl
```

The `other` arm of the same `match` uses a scissored `glClear`, which swgl
implements (`gl.cc:2750`). Everything else about WebRender-on-swgl works
untouched, because WebRender autodetects it from `glGetString(GL_RENDERER)`.

Worked around downstream by patching `servo-paint` via `[patch.crates-io]`
(`third_party/servo-paint`), which is a poor trade for one boolean — hence
this report. A `RenderingContext`-level hint, or simply deriving it from
`is_software_webrender` inside WebRender, would remove the need for a fork.

---

## 7. `enable_dithering` asks for a gradient shader variant swgl does not build

**Observed** — with the CPU rasterizer (`swgl` 0.70) the process *aborts* the
first time a CSS gradient is painted, which in practice means "on scroll":

```
src/gl.cc:1523: void BindAttribLocation(GLuint, GLuint, char*): Assertion `p.impl' failed.
```

**Cause** — `servo-paint-0.5.0/painter.rs:241` sets `enable_dithering: true`.
WebRender then requests the gradient shader with the `DITHERING` feature
(`webrender-0.70.0/src/renderer/shade.rs:866-875`):

```rust
let ps_quad_gradient = loader.create_shader(
    ShaderKind::Primitive,
    "ps_quad_gradient",
    if options.enable_dithering { &[DITHERING_FEATURE] } else { &[] },
    &shader_list,
)?;
```

so the lookup key is `"ps_quad_gradient DITHERING"`. But `swgl`'s build.rs
enumerates its shader set from
`webrender_build::get_shader_features(GL | DUAL_SOURCE_BLENDING |
ADVANCED_BLEND_EQUATION | DEBUG)`, and that set contains only the bare
`ps_quad_gradient` — the generated `OUT_DIR` holds `ps_quad_gradient.h` and no
`ps_quad_gradient_DITHERING.h`, next to variants like
`ps_quad_textured_TEXTURE_2D.h` that *are* expanded. `load_shader` returns
`nullptr`, the program keeps a null `impl`, and the next
`glBindAttribLocation` asserts.

Note the failure mode is a hard `abort()`: it does not unwind, so it runs no
Rust panic hook, and on Windows it does not raise a structured exception
either, so `SetUnhandledExceptionFilter` never sees it. An embedded Servo
simply takes the host application down with no output at all.

**Fix** — either `get_shader_features` should include the `DITHERING` variant
of `ps_quad_gradient` (so swgl builds it), or `enable_dithering` should be
forced off when the GL is software. Worked around downstream with
`enable_dithering: false`, which costs some banding in gradients.

This is the second place where swgl 0.70's generated shader table and
webrender 0.70's runtime requests disagree (see issue 6 for `GL_ALWAYS`), so
it may be worth a general audit rather than two point fixes.

---

## Notes

Issues 1, 3 and 4 are each enough on their own to stop the stock PDF.js viewer
from working; issue 2 makes it unusably slow; issue 5 hides issue 1. All five
are worked around downstream, so this is a report rather than a request for
urgency — but PDF.js is a good canary for embedding real-world web apps, and
these are the four platform gaps it hits.

Issue 6 stands apart: it is not a platform gap but a hard-coded renderer
option, and unlike the others it cannot be worked around from the embedding
API at all — only by forking `servo-paint`.
