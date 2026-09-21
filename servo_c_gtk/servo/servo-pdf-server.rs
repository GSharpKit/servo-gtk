//! Loopback HTTP server for hosting the PDF.js viewer and local documents.
//!
//! Servo — like every other engine — treats `file://` as a hostile origin:
//! module scripts, dedicated workers, `fetch()` and byte-range reads all
//! behave differently (or not at all) there. PDF.js depends on every one of
//! those, so the supported way to embed it is to serve the viewer and the
//! documents over a real HTTP origin. This module provides exactly that much
//! HTTP and no more:
//!
//! * binds `127.0.0.1:0` (an ephemeral port) — never a routable interface;
//! * mounts the PDF.js distribution at `/pdfjs` and, optionally, a document
//!   directory at `/documents`;
//! * serves `.mjs` / `.js` as `text/javascript` so module scripts and the
//!   PDF.js worker actually load;
//! * answers `Range` requests with `206 Partial Content`, which is what lets
//!   PDF.js stream a large PDF instead of downloading it up front;
//! * marks the immutable PDF.js build assets cacheable for a year and the
//!   documents `no-store`;
//! * sends a `Content-Security-Policy` sized for the viewer (workers, blobs,
//!   wasm) on the HTML it serves.
//!
//! # Access control
//!
//! A loopback socket is reachable by every process on the machine and, for
//! `GET`s, by any page the user happens to have open. Every URL is therefore
//! scoped by a 128-bit capability token generated from OS entropy at startup
//! and carried as the first path segment (`/<token>/pdfjs/...`). Requests
//! without it get a `404`, and requests whose `Host` header is not a loopback
//! literal get a `421` (DNS-rebinding defence). If OS entropy is unavailable
//! the server refuses to start rather than falling back to a guessable token.
//!
//! # Threading
//!
//! Unlike the rest of this crate, the server is entirely self-contained: it
//! touches no Servo object, runs on its own threads and may be driven from any
//! thread. Only the handle itself must not be used concurrently.

use std::collections::HashMap;
use std::ffi::{CStr, CString, c_char};
use std::fs::{File, Metadata};
use std::io::{self, BufRead, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, RwLock};
use std::thread::JoinHandle;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// Longest tolerated request line (method + target + version).
const MAX_REQUEST_LINE: usize = 8 * 1024;
/// Longest tolerated single header line.
const MAX_HEADER_LINE: usize = 8 * 1024;
/// Upper bound on the combined size of all header lines in one request.
const MAX_HEADER_BYTES: usize = 32 * 1024;
/// Upper bound on the number of header lines in one request.
const MAX_HEADERS: usize = 64;
/// How long an idle keep-alive connection is held open.
const KEEP_ALIVE_TIMEOUT: Duration = Duration::from_secs(10);
/// How long a single response write may stall before the peer is dropped.
const WRITE_TIMEOUT: Duration = Duration::from_secs(30);
/// Concurrent connections served before new ones are refused. PDF.js opens a
/// handful of parallel range requests; this leaves generous headroom while
/// bounding the number of threads a local peer can force us to spawn.
const MAX_CONNECTIONS: usize = 16;

/// URL prefix (below the capability token) of the PDF.js distribution mount.
const PDFJS_PREFIX: &str = "/pdfjs/";
/// URL prefix (below the capability token) of the document mount.
const DOCUMENTS_PREFIX: &str = "/documents/";
/// Default location of the PDF.js generic viewer inside the distribution.
const DEFAULT_VIEWER_PATH: &str = "/pdfjs/web/viewer.html";
/// Synthetic route serving the boot script injected into every PDF.js page:
/// the engine compatibility shims plus any embedder preferences. Named so it
/// cannot collide with a real file in the PDF.js distribution.
const BOOTSTRAP_SCRIPT_PATH: &str = "/pdfjs/__servo_embed_bootstrap.js";

/// CSP for the viewer document. PDF.js needs workers (and spawns them from
/// blob URLs when the worker file is cross-origin), blob/data images for
/// rendered pages and thumbnails, data: fonts for embedded PDF fonts, and
/// `wasm-unsafe-eval` for the optional WASM image decoders.
const VIEWER_CSP: &str = "default-src 'self'; \
     script-src 'self' 'wasm-unsafe-eval'; \
     worker-src 'self' blob:; \
     style-src 'self' 'unsafe-inline'; \
     img-src 'self' blob: data:; \
     font-src 'self' data:; \
     connect-src 'self' blob:; \
     object-src 'none'; \
     base-uri 'self'; \
     form-action 'none'; \
     frame-ancestors 'self'";

// ---------------------------------------------------------------------------
// Handle
// ---------------------------------------------------------------------------

/// Immutable (apart from the viewer preferences) state shared with every
/// connection thread.
struct ServerConfig {
    /// Capability token that must be the first path segment of every request.
    token: String,
    /// Canonical root of the PDF.js distribution, mounted at [`PDFJS_PREFIX`].
    pdfjs_root: PathBuf,
    /// Canonical root of the document directory, mounted at
    /// [`DOCUMENTS_PREFIX`], or `None` when the embedder did not supply one.
    documents_root: Option<PathBuf>,
    /// Individually published documents, `<name>` -> canonical path, served at
    /// `/documents/<name>`. Lets an embedder expose one picked file without
    /// mounting the directory that happens to contain it.
    documents: RwLock<HashMap<String, PathBuf>>,
    /// JSON object of PDF.js viewer preferences to seed, or `None` to serve
    /// the distribution's HTML untouched.
    viewer_preferences: RwLock<Option<String>>,
    /// Whether to add `'unsafe-inline'` to the `style-src` of the page's own
    /// `<meta>` CSP. Off by default; see
    /// [`servo_pdf_server_set_relax_style_csp`].
    relax_style_csp: AtomicBool,
}

/// A running loopback HTTP server. Created by [`servo_pdf_server_start`] and
/// shut down by [`servo_pdf_server_stop`].
pub struct ServoPdfServerHandle {
    /// Scheme, host and port, e.g. `http://127.0.0.1:41235`.
    origin: String,
    /// The address the acceptor is bound to; also used to wake it on shutdown.
    address: SocketAddr,
    config: Arc<ServerConfig>,
    shutdown: Arc<AtomicBool>,
    acceptor: Option<JoinHandle<()>>,
}

impl ServoPdfServerHandle {
    /// Absolute URL for a server-relative `path` (which must start with `/`),
    /// with the capability token spliced in.
    fn url_for(&self, path: &str) -> String {
        format!("{}/{}{}", self.origin, self.config.token, path)
    }
}

impl Drop for ServoPdfServerHandle {
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::SeqCst);
        // `accept()` blocks and has no portable interruption, so wake it with a
        // throwaway connection; it re-checks the flag and returns.
        if let Ok(stream) = TcpStream::connect_timeout(&self.address, Duration::from_millis(500)) {
            let _ = stream.shutdown(Shutdown::Both);
        }
        if let Some(acceptor) = self.acceptor.take() {
            let _ = acceptor.join();
        }
    }
}

/// Bind a loopback listener and start accepting.
///
/// Both roots are canonicalised up front: a missing or unreadable directory is
/// an error rather than a server that 404s everything.
fn start_server(
    pdfjs_dir: &Path,
    documents_dir: Option<&Path>,
) -> io::Result<ServoPdfServerHandle> {
    let pdfjs_root = pdfjs_dir.canonicalize()?;
    if !pdfjs_root.is_dir() {
        return Err(io::Error::other("PDF.js directory is not a directory"));
    }
    let documents_root = match documents_dir {
        Some(dir) => {
            let root = dir.canonicalize()?;
            if !root.is_dir() {
                return Err(io::Error::other("document directory is not a directory"));
            }
            Some(root)
        },
        None => None,
    };

    // Fail closed: without OS entropy the token would be guessable, which is
    // the only thing keeping other local processes out.
    let token = random_token()?;

    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0))?;
    let address = listener.local_addr()?;

    let config = Arc::new(ServerConfig {
        token,
        pdfjs_root,
        documents_root,
        documents: RwLock::new(HashMap::new()),
        viewer_preferences: RwLock::new(None),
        relax_style_csp: AtomicBool::new(false),
    });
    let shutdown = Arc::new(AtomicBool::new(false));

    let acceptor = {
        let config = config.clone();
        let shutdown = shutdown.clone();
        std::thread::Builder::new()
            .name("servo-pdf-http".to_owned())
            .spawn(move || accept_loop(listener, config, shutdown))?
    };

    Ok(ServoPdfServerHandle {
        origin: format!("http://127.0.0.1:{}", address.port()),
        address,
        config,
        shutdown,
        acceptor: Some(acceptor),
    })
}

fn accept_loop(listener: TcpListener, config: Arc<ServerConfig>, shutdown: Arc<AtomicBool>) {
    let live = Arc::new(AtomicUsize::new(0));
    loop {
        let stream = match listener.accept() {
            Ok((stream, _peer)) => stream,
            Err(_) => {
                if shutdown.load(Ordering::SeqCst) {
                    return;
                }
                // Transient accept failures (EMFILE, ECONNABORTED) must not
                // turn into a busy loop.
                std::thread::sleep(Duration::from_millis(10));
                continue;
            },
        };
        if shutdown.load(Ordering::SeqCst) {
            let _ = stream.shutdown(Shutdown::Both);
            return;
        }
        if live.load(Ordering::SeqCst) >= MAX_CONNECTIONS {
            let _ = stream.shutdown(Shutdown::Both);
            continue;
        }
        live.fetch_add(1, Ordering::SeqCst);
        let config = config.clone();
        let connection_live = live.clone();
        let spawned = std::thread::Builder::new()
            .name("servo-pdf-http-conn".to_owned())
            .spawn(move || {
                let _ = serve_connection(stream, &config);
                connection_live.fetch_sub(1, Ordering::SeqCst);
            });
        if spawned.is_err() {
            live.fetch_sub(1, Ordering::SeqCst);
        }
    }
}

// ---------------------------------------------------------------------------
// Connection / request handling
// ---------------------------------------------------------------------------

struct Request {
    method: String,
    target: String,
    /// Header names lower-cased; values trimmed.
    headers: Vec<(String, String)>,
    keep_alive: bool,
}

impl Request {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(key, _)| key == name)
            .map(|(_, value)| value.as_str())
    }
}

fn serve_connection(stream: TcpStream, config: &ServerConfig) -> io::Result<()> {
    let _ = stream.set_nodelay(true);
    let _ = stream.set_read_timeout(Some(KEEP_ALIVE_TIMEOUT));
    let _ = stream.set_write_timeout(Some(WRITE_TIMEOUT));

    let mut reader = BufReader::new(stream.try_clone()?);
    let mut writer = BufWriter::new(stream);

    loop {
        let request = match read_request(&mut reader) {
            Ok(Some(request)) => request,
            // Clean EOF: the peer closed an idle keep-alive connection.
            Ok(None) => return Ok(()),
            Err(status) => {
                write_response(&mut writer, error_response(status), false, false)?;
                return writer.flush();
            },
        };

        let head_only = request.method == "HEAD";
        let response = handle_request(&request, config);
        let keep_alive = request.keep_alive && !response.close;
        write_response(&mut writer, response, head_only, keep_alive)?;
        writer.flush()?;

        if !keep_alive {
            return Ok(());
        }
    }
}

/// Read one request. `Ok(None)` means the peer closed cleanly before sending
/// anything; `Err(status)` is the HTTP status to reject it with.
fn read_request<R: BufRead>(reader: &mut R) -> Result<Option<Request>, u16> {
    let line = match read_line_limited(reader, MAX_REQUEST_LINE) {
        Ok(Some(line)) => line,
        Ok(None) => return Ok(None),
        Err(_) => return Err(414),
    };
    // Tolerate the stray CRLF some clients send before a pipelined request.
    let line = if line.is_empty() {
        match read_line_limited(reader, MAX_REQUEST_LINE) {
            Ok(Some(line)) => line,
            Ok(None) => return Ok(None),
            Err(_) => return Err(414),
        }
    } else {
        line
    };

    let mut parts = line.split(' ');
    let (Some(method), Some(target), Some(version), None) =
        (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return Err(400);
    };
    if !version.starts_with("HTTP/1.") {
        return Err(505);
    }

    let mut headers: Vec<(String, String)> = Vec::new();
    let mut header_bytes = 0usize;
    loop {
        let line = match read_line_limited(reader, MAX_HEADER_LINE) {
            Ok(Some(line)) => line,
            Ok(None) => return Err(400),
            Err(_) => return Err(431),
        };
        if line.is_empty() {
            break;
        }
        header_bytes += line.len();
        if headers.len() >= MAX_HEADERS || header_bytes > MAX_HEADER_BYTES {
            return Err(431);
        }
        let Some((name, value)) = line.split_once(':') else {
            return Err(400);
        };
        headers.push((name.trim().to_ascii_lowercase(), value.trim().to_owned()));
    }

    // We never read a request body, so a request that announces one would
    // desynchronise the connection. Reject it instead of guessing.
    let has_body = headers.iter().any(|(name, value)| {
        (name == "content-length" && value != "0") || name == "transfer-encoding"
    });
    if has_body {
        return Err(400);
    }

    let connection = headers
        .iter()
        .find(|(name, _)| name == "connection")
        .map(|(_, value)| value.to_ascii_lowercase());
    let keep_alive = match connection.as_deref() {
        Some(value) if value.split(',').any(|token| token.trim() == "close") => false,
        Some(value) if value.split(',').any(|token| token.trim() == "keep-alive") => true,
        _ => version == "HTTP/1.1",
    };

    Ok(Some(Request {
        method: method.to_owned(),
        target: target.to_owned(),
        headers,
        keep_alive,
    }))
}

/// Read one CRLF-terminated line, capped at `max` bytes. `Ok(None)` is EOF.
fn read_line_limited<R: BufRead>(reader: &mut R, max: usize) -> io::Result<Option<String>> {
    let mut buffer = Vec::new();
    let read = reader
        .by_ref()
        .take(max as u64 + 1)
        .read_until(b'\n', &mut buffer)?;
    if read == 0 {
        return Ok(None);
    }
    if buffer.len() > max || !buffer.ends_with(b"\n") {
        return Err(io::Error::other("header line too long"));
    }
    while matches!(buffer.last(), Some(b'\n' | b'\r')) {
        buffer.pop();
    }
    String::from_utf8(buffer)
        .map(Some)
        .map_err(|_| io::Error::other("header line is not UTF-8"))
}

fn handle_request(request: &Request, config: &ServerConfig) -> Response {
    // DNS-rebinding defence: a page on any origin can resolve a hostname to
    // 127.0.0.1, but it cannot forge the Host header.
    match request.header("host") {
        Some(host) if is_loopback_host(host) => {},
        _ => return error_response(421),
    }

    if request.method != "GET" && request.method != "HEAD" {
        return error_response(405).header("Allow", "GET, HEAD").close();
    }

    // Strip the query and any (non-conforming) fragment.
    let target = request.target.split(['?', '#']).next().unwrap_or("");
    let Some(decoded) = percent_decode(target) else {
        return error_response(400);
    };
    if decoded.contains('\0') || !decoded.starts_with('/') {
        return error_response(400);
    }

    // Every URL is scoped by the capability token; anything else is a 404 so
    // we leak nothing about what is mounted.
    let Some(rest) = decoded.strip_prefix('/') else {
        return error_response(404);
    };
    let (token, path) = match rest.split_once('/') {
        Some((token, path)) => (token, format!("/{path}")),
        None => (rest, String::from("/")),
    };
    if !constant_time_eq(token.as_bytes(), config.token.as_bytes()) {
        return error_response(404);
    }

    if path == BOOTSTRAP_SCRIPT_PATH {
        return bootstrap_script_response(config);
    }

    let (root, relative) = if let Some(relative) = path.strip_prefix(PDFJS_PREFIX) {
        (&config.pdfjs_root, relative)
    } else if let Some(relative) = path.strip_prefix(DOCUMENTS_PREFIX) {
        // An individually published document wins over the directory mount and
        // is served by the canonical path the embedder gave us, so nothing is
        // resolved out of the URL at all.
        if let Some(published) = published_document(config, relative) {
            return file_response(&published, &path, request, config);
        }
        match config.documents_root.as_ref() {
            Some(root) => (root, relative),
            None => return error_response(404),
        }
    } else {
        return error_response(404);
    };

    let Some(resolved) = safe_join(root, relative) else {
        return error_response(404);
    };

    file_response(&resolved, &path, request, config)
}

/// Serve a resolved file, honouring `If-None-Match`, `If-Range` and `Range`.
fn file_response(
    resolved: &Path,
    url_path: &str,
    request: &Request,
    config: &ServerConfig,
) -> Response {
    let Ok(mut file) = File::open(resolved) else {
        return error_response(404);
    };
    let Ok(metadata) = file.metadata() else {
        return error_response(500);
    };
    if !metadata.is_file() {
        return error_response(404);
    }

    let content_type = content_type_for(resolved);
    let cache_control = cache_control_for(url_path);
    let etag = entity_tag(&metadata);
    let last_modified = metadata.modified().ok().map(http_date);

    // Stylesheets that lean on CSS masking need the icon fallback woven in, so
    // their bytes no longer match the file on disk either.
    if content_type.starts_with("text/css") && url_path.starts_with(PDFJS_PREFIX) {
        let mut css = String::new();
        if file.read_to_string(&mut css).is_err() {
            return error_response(500);
        }
        if css.contains("mask-image:") {
            let css = inline_mask_icon_fallback(&css);
            return Response::new(200)
                .header("Content-Type", content_type)
                .header("Cache-Control", "no-store")
                .header("Referrer-Policy", "no-referrer")
                .header("X-Content-Type-Options", "nosniff")
                .with_body(Body::Bytes(css.into_bytes()));
        }
        // No masking in this sheet: fall through and serve the file as-is,
        // cacheable and range-able like any other asset.
        if file.seek(SeekFrom::Start(0)).is_err() {
            return error_response(500);
        }
    }

    // PDF.js HTML is always rewritten to carry the boot script, so its bytes no
    // longer match the file on disk: no ETag, no range, no caching.
    if content_type.starts_with("text/html") && url_path.starts_with(PDFJS_PREFIX) {
        let mut html = String::new();
        if file.read_to_string(&mut html).is_err() {
            return error_response(500);
        }
        if config.relax_style_csp.load(Ordering::Relaxed) {
            html = relax_style_csp(&html);
        }
        let script_src = format!("/{}{}", config.token, BOOTSTRAP_SCRIPT_PATH);
        let html = inject_script(&html, &script_src);
        return Response::new(200)
            .header("Content-Type", content_type)
            .header("Cache-Control", "no-store")
            .header("Content-Security-Policy", VIEWER_CSP)
            .header("Referrer-Policy", "no-referrer")
            .header("X-Content-Type-Options", "nosniff")
            .with_body(Body::Bytes(html.into_bytes()));
    }

    let mut response = Response::new(200)
        .header("Content-Type", content_type)
        .header("Cache-Control", cache_control)
        .header("Accept-Ranges", "bytes")
        .header("ETag", &etag)
        .header("Referrer-Policy", "no-referrer")
        .header("X-Content-Type-Options", "nosniff");
    if let Some(last_modified) = last_modified {
        response = response.header("Last-Modified", &last_modified);
    }
    if content_type.starts_with("text/html") {
        response = response.header("Content-Security-Policy", VIEWER_CSP);
    }

    if let Some(if_none_match) = request.header("if-none-match") {
        if etag_matches(if_none_match, &etag) {
            return response.status(304);
        }
    }

    let length = metadata.len();
    // A stale `If-Range` means the client's cached prefix is invalid, so the
    // range must be ignored and the whole entity returned.
    let range_applies = match request.header("if-range") {
        Some(value) => etag_matches(value, &etag),
        None => true,
    };
    let range = match request.header("range") {
        Some(spec) if range_applies => parse_range(spec, length),
        _ => RangeSpec::Whole,
    };

    match range {
        RangeSpec::Whole => response.with_body(Body::File { file, length }),
        RangeSpec::Unsatisfiable => error_response(416)
            .header("Content-Range", &format!("bytes */{length}"))
            .header("Accept-Ranges", "bytes"),
        RangeSpec::Partial { start, end } => {
            if file.seek(SeekFrom::Start(start)).is_err() {
                return error_response(500);
            }
            response
                .status(206)
                .header("Content-Range", &format!("bytes {start}-{end}/{length}"))
                .with_body(Body::File {
                    file,
                    length: end - start + 1,
                })
        },
    }
}

/// Look up an individually published document. Only a single path segment can
/// name one, so a published document can never shadow a subdirectory.
fn published_document(config: &ServerConfig, relative: &str) -> Option<PathBuf> {
    if relative.is_empty() || relative.contains('/') {
        return None;
    }
    config.documents.read().ok()?.get(relative).cloned()
}

/// Whether `name` is usable as the last segment of `/documents/<name>`.
fn is_publishable_name(name: &str) -> bool {
    !name.is_empty() && name != "." && name != ".." && !name.contains(['/', '\\', '\0'])
}

// ---------------------------------------------------------------------------
// Viewer preference bootstrap
// ---------------------------------------------------------------------------

/// Engine compatibility shims, prepended to every boot script.
///
/// PDF.js 6.x uses the TC39 `Map.prototype.getOrInsert` proposal in its
/// `EventBus`. Servo's SpiderMonkey does not expose it yet, so
/// `PDFFindController`'s constructor throws, `initialize()` rejects, and the
/// viewer renders its chrome and nothing else — with no console output,
/// because the rejection is never reported. Defining the two methods when they
/// are missing is enough to make the stock distribution work.
const ENGINE_SHIMS: &str = r#"(function () {
  function define(proto, name, fn) {
    if (proto && typeof proto[name] !== "function") {
      Object.defineProperty(proto, name, {
        value: fn, writable: true, enumerable: false, configurable: true
      });
    }
  }
  function getOrInsert(key, value) {
    if (!this.has(key)) { this.set(key, value); }
    return this.get(key);
  }
  function getOrInsertComputed(key, callback) {
    if (!this.has(key)) { this.set(key, callback(key)); }
    return this.get(key);
  }
  define(Map.prototype, "getOrInsert", getOrInsert);
  define(Map.prototype, "getOrInsertComputed", getOrInsertComputed);
  define(WeakMap.prototype, "getOrInsert", getOrInsert);
  define(WeakMap.prototype, "getOrInsertComputed", getOrInsertComputed);
  // An unreported rejection is what made the failure above invisible; surface
  // any future one on the console instead.
  window.addEventListener("unhandledrejection", function (event) {
    var reason = event.reason;
    console.error("Unhandled rejection: " + ((reason && (reason.stack || reason.message)) || reason));
  });
})();
"#;

/// Viewer options this server turns off by default because Servo cannot render
/// the feature correctly. The embedder can switch any of them back on through
/// [`servo_pdf_server_set_viewer_preferences`].
///
/// * `enableAutoLinking` — PDF.js 6.x detects URLs in page text and adds a
///   link annotation for each. One annotation can span several text runs, so
///   PDF.js emits a single deliberately oversized element (heights over 200%
///   of the page are normal) and clips it back to the real runs with
///   `clip-path: url(#…)`. Servo parses that property but applies neither the
///   paint clip nor the hit-test clip, so the element stays full size: the
///   whole page picks up PDF.js's yellow link-hover tint, and — worse — a
///   click anywhere on the page follows the detected URL.
fn servo_default_preferences() -> serde_json::Map<String, serde_json::Value> {
    let mut defaults = serde_json::Map::new();
    defaults.insert("enableAutoLinking".to_owned(), serde_json::Value::Bool(false));
    defaults
}

/// The options to apply to the viewer: this server's defaults, with the
/// embedder's preferences layered on top so they always win.
fn viewer_overrides(config: &ServerConfig) -> String {
    let mut overrides = servo_default_preferences();

    let preferences = config
        .viewer_preferences
        .read()
        .ok()
        .and_then(|guard| guard.clone());
    if let Some(json) = preferences {
        // Already validated as an object by the setter; ignore it if that ever
        // stops holding rather than dropping the defaults too.
        if let Ok(serde_json::Value::Object(embedder)) = serde_json::from_str(&json) {
            for (name, value) in embedder {
                overrides.insert(name, value);
            }
        }
    }

    serde_json::Value::Object(overrides).to_string()
}

/// Build the boot script: compatibility shims, then the viewer options.
fn bootstrap_script(config: &ServerConfig) -> String {
    let mut script = String::from(ENGINE_SHIMS);
    let json = viewer_overrides(config);

    // Two mechanisms, because PDF.js splits its settings in two:
    //
    //  * preference-kind options (enableScripting, sidebarViewOnLoad, …) are
    //    read out of this localStorage key while the viewer boots, and they
    //    overwrite anything set earlier, so they have to go in storage;
    //  * everything else (maxCanvasPixels, disableAutoFetch, …) lives only in
    //    AppOptions, which does not exist until viewer.mjs has run — PDF.js
    //    fires `webviewerloaded` for exactly this kind of embedder override.
    //
    // Applying the same object through both paths is safe: each side ignores
    // the names it does not know.
    script.push_str(&format!(
        "(function () {{\n  \
           var overrides = {json};\n  \
           try {{\n    \
             var key = \"pdfjs.preferences\";\n    \
             var current = {{}};\n    \
             try {{ current = JSON.parse(window.localStorage.getItem(key)) || {{}}; }} \
                  catch (e) {{ current = {{}}; }}\n    \
             for (var name in overrides) {{\n      \
               if (Object.prototype.hasOwnProperty.call(overrides, name)) {{\n        \
                 current[name] = overrides[name];\n      \
               }}\n    \
             }}\n    \
             window.localStorage.setItem(key, JSON.stringify(current));\n  \
           }} catch (e) {{\n    \
             /* No storage: the AppOptions pass below still applies. */\n  \
           }}\n  \
           document.addEventListener(\"webviewerloaded\", function () {{\n    \
             var options = window.PDFViewerApplicationOptions;\n    \
             if (!options) {{ return; }}\n    \
             for (var name in overrides) {{\n      \
               if (Object.prototype.hasOwnProperty.call(overrides, name)) {{\n        \
                 try {{ options.set(name, overrides[name]); }} catch (e) {{ /* unknown */ }}\n      \
               }}\n    \
             }}\n  \
           }}, {{ once: true }});\n\
         }})();\n"
    ));
    script
}

fn bootstrap_script_response(config: &ServerConfig) -> Response {
    Response::new(200)
        .header("Content-Type", "text/javascript; charset=utf-8")
        .header("Cache-Control", "no-store")
        .header("X-Content-Type-Options", "nosniff")
        .with_body(Body::Bytes(bootstrap_script(config).into_bytes()))
}

/// Add `'unsafe-inline'` to every `style-src` directive in the document, so
/// Servo stops running a per-restyle CSP check on UA shadow content.
///
/// Deliberately narrow: it only widens an existing `style-src`, so a document
/// that never restricted inline styles is untouched, and no other directive is
/// altered.
fn relax_style_csp(html: &str) -> String {
    let mut out = String::with_capacity(html.len() + 64);
    let mut rest = html;
    while let Some(index) = rest.find("style-src ") {
        let (before, after) = rest.split_at(index + "style-src ".len());
        out.push_str(before);
        // The directive runs to the next `;` or to the end of the attribute.
        // Source expressions are single-quoted (`'self'`), so only `;`, the
        // attribute's own double quote, and `>` can terminate it.
        let end = after
            .find([';', '"', '>'])
            .unwrap_or(after.len());
        let (directive, tail) = after.split_at(end);
        out.push_str(directive);
        if !directive.contains("'unsafe-inline'") {
            out.push_str(" 'unsafe-inline'");
        }
        rest = tail;
    }
    out.push_str(rest);
    out
}

/// Give every `mask-image` declaration a `background-image` equivalent.
///
/// PDF.js draws all ~90 of its toolbar icons as a coloured box masked to the
/// shape of an SVG (`background-color: …; mask-image: var(--icon)`). Servo
/// implements no CSS masking at all — `CSS.supports("mask-image", …)` is false
/// and the declaration is dropped at parse time — so the mask never applies
/// and every icon renders as the bare 16x16 coloured block underneath it.
///
/// The icon SVGs draw their glyph with `fill="black"`, so the same URL works
/// directly as a background image. Rewriting the declaration in place keeps
/// this free of CSS parsing: whatever rule contained the mask now also sets an
/// equivalent background, whether the rule was flat or nested, without ever
/// needing to know its selector.
///
/// The background colour has to be cleared or the block would still be painted
/// underneath the glyph. An engine that *does* mask therefore renders these
/// icons in the glyph's own colour rather than the theme's — acceptable here,
/// since this server exists to host PDF.js for Servo.
fn inline_mask_icon_fallback(css: &str) -> String {
    const NEEDLE: &str = "mask-image:";
    let mut out = String::with_capacity(css.len() + css.len() / 8);
    let mut rest = css;

    while let Some(index) = rest.find(NEEDLE) {
        let (before, after) = rest.split_at(index + NEEDLE.len());
        out.push_str(before);
        rest = after;

        // `-webkit-mask-image:` also ends with the needle; only the standard
        // property should be augmented, or every icon would get two copies.
        let prefix = &before[..before.len() - NEEDLE.len()];
        let prefixed = prefix
            .chars()
            .next_back()
            .is_some_and(|c| c == '-' || c.is_ascii_alphanumeric());

        // The declaration runs to the next `;` or to the end of the block.
        let end = rest.find([';', '}']).unwrap_or(rest.len());
        let (value, tail) = rest.split_at(end);
        out.push_str(value);
        rest = tail;

        if !prefixed && !value.trim().is_empty() {
            out.push_str(&format!(
                ";background-image:{};\
                 background-color:transparent;\
                 background-repeat:no-repeat;\
                 background-position:center;\
                 background-size:contain",
                value.trim()
            ));
        }
    }

    out.push_str(rest);
    out
}

/// Insert a classic `<script src=…>` just before `</head>`, or at the very
/// start when the document has no head.
fn inject_script(html: &str, src: &str) -> String {
    let tag = format!("<script src=\"{src}\"></script>");
    // `to_ascii_lowercase` is byte-length preserving, so the index is valid in
    // the original string too.
    match html.to_ascii_lowercase().find("</head>") {
        Some(index) => {
            let mut out = String::with_capacity(html.len() + tag.len());
            out.push_str(&html[..index]);
            out.push_str(&tag);
            out.push_str(&html[index..]);
            out
        },
        None => format!("{tag}{html}"),
    }
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

enum Body {
    Empty,
    Bytes(Vec<u8>),
    File { file: File, length: u64 },
}

impl Body {
    fn length(&self) -> u64 {
        match self {
            Body::Empty => 0,
            Body::Bytes(bytes) => bytes.len() as u64,
            Body::File { length, .. } => *length,
        }
    }
}

struct Response {
    status: u16,
    headers: Vec<(&'static str, String)>,
    body: Body,
    /// Force the connection shut after this response.
    close: bool,
}

impl Response {
    fn new(status: u16) -> Self {
        Response {
            status,
            headers: Vec::new(),
            body: Body::Empty,
            close: false,
        }
    }

    fn status(mut self, status: u16) -> Self {
        self.status = status;
        self
    }

    fn header(mut self, name: &'static str, value: &str) -> Self {
        self.headers.push((name, value.to_owned()));
        self
    }

    fn with_body(mut self, body: Body) -> Self {
        self.body = body;
        self
    }

    fn close(mut self) -> Self {
        self.close = true;
        self
    }
}

fn error_response(status: u16) -> Response {
    let text = format!("{} {}\n", status, reason_phrase(status));
    Response::new(status)
        .header("Content-Type", "text/plain; charset=utf-8")
        .header("Cache-Control", "no-store")
        .header("X-Content-Type-Options", "nosniff")
        .with_body(Body::Bytes(text.into_bytes()))
}

fn reason_phrase(status: u16) -> &'static str {
    match status {
        200 => "OK",
        206 => "Partial Content",
        304 => "Not Modified",
        400 => "Bad Request",
        404 => "Not Found",
        405 => "Method Not Allowed",
        414 => "URI Too Long",
        416 => "Range Not Satisfiable",
        421 => "Misdirected Request",
        431 => "Request Header Fields Too Large",
        500 => "Internal Server Error",
        505 => "HTTP Version Not Supported",
        _ => "Error",
    }
}

fn write_response<W: Write>(
    writer: &mut W,
    response: Response,
    head_only: bool,
    keep_alive: bool,
) -> io::Result<()> {
    let mut head = format!(
        "HTTP/1.1 {} {}\r\nDate: {}\r\n",
        response.status,
        reason_phrase(response.status),
        http_date(SystemTime::now())
    );
    // A 304 carries no body and no framing of its own.
    if response.status != 304 {
        head.push_str(&format!("Content-Length: {}\r\n", response.body.length()));
    }
    head.push_str(if keep_alive {
        "Connection: keep-alive\r\n"
    } else {
        "Connection: close\r\n"
    });
    for (name, value) in &response.headers {
        head.push_str(&format!("{name}: {value}\r\n"));
    }
    head.push_str("\r\n");
    writer.write_all(head.as_bytes())?;

    if head_only || response.status == 304 {
        return Ok(());
    }
    match response.body {
        Body::Empty => Ok(()),
        Body::Bytes(bytes) => writer.write_all(&bytes),
        // `file` was already positioned at the start of the range.
        Body::File { file, length } => io::copy(&mut file.take(length), writer).map(|_| ()),
    }
}

// ---------------------------------------------------------------------------
// Ranges, MIME types, caching
// ---------------------------------------------------------------------------

enum RangeSpec {
    Whole,
    Partial { start: u64, end: u64 },
    Unsatisfiable,
}

/// Parse a single-range `Range` header. Multi-range requests fall back to the
/// whole entity, which is always a valid response and is never what PDF.js
/// asks for anyway.
fn parse_range(spec: &str, length: u64) -> RangeSpec {
    let Some(set) = spec.trim().strip_prefix("bytes=") else {
        return RangeSpec::Whole;
    };
    if set.contains(',') {
        return RangeSpec::Whole;
    }
    let Some((first, last)) = set.trim().split_once('-') else {
        return RangeSpec::Whole;
    };

    if first.is_empty() {
        // Suffix form: the final N bytes.
        let Ok(suffix) = last.parse::<u64>() else {
            return RangeSpec::Whole;
        };
        if suffix == 0 || length == 0 {
            return RangeSpec::Unsatisfiable;
        }
        let start = length.saturating_sub(suffix);
        return RangeSpec::Partial {
            start,
            end: length - 1,
        };
    }

    let Ok(start) = first.parse::<u64>() else {
        return RangeSpec::Whole;
    };
    if start >= length {
        return RangeSpec::Unsatisfiable;
    }
    let end = if last.is_empty() {
        length - 1
    } else {
        match last.parse::<u64>() {
            Ok(end) => end.min(length - 1),
            Err(_) => return RangeSpec::Whole,
        }
    };
    if end < start {
        return RangeSpec::Unsatisfiable;
    }
    RangeSpec::Partial { start, end }
}

/// Weak-comparison match of an `If-None-Match` / `If-Range` value against our
/// entity tag.
fn etag_matches(header: &str, etag: &str) -> bool {
    header.split(',').any(|candidate| {
        let candidate = candidate.trim();
        candidate == "*" || candidate.trim_start_matches("W/") == etag
    })
}

/// Format an instant as an IMF-fixdate (`Sun, 06 Nov 1994 08:49:37 GMT`), the
/// only date format HTTP servers are allowed to generate. Instants before the
/// Unix epoch (only reachable from a bogus mtime) clamp to the epoch.
fn http_date(time: SystemTime) -> String {
    const DAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    const MONTHS: [&str; 12] = [
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    ];

    let seconds = time
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or(0);
    let days = (seconds / 86_400) as i64;
    let seconds_of_day = seconds % 86_400;
    let weekday = ((days + 4).rem_euclid(7)) as usize; // 1970-01-01 was a Thursday.

    // days-from-civil, inverted (Howard Hinnant's algorithm): shift the epoch to
    // 0000-03-01 so leap days land at the end of the 400-year era.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let day_of_era = z.rem_euclid(146_097);
    let year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let shifted_month = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    let month = if shifted_month < 10 {
        shifted_month + 3
    } else {
        shifted_month - 9
    };
    let year = year_of_era + era * 400 + i64::from(month <= 2);

    format!(
        "{}, {:02} {} {} {:02}:{:02}:{:02} GMT",
        DAYS[weekday],
        day,
        MONTHS[(month - 1) as usize],
        year,
        seconds_of_day / 3600,
        (seconds_of_day % 3600) / 60,
        seconds_of_day % 60,
    )
}

/// A strong entity tag derived from the file's size and modification time.
fn entity_tag(metadata: &Metadata) -> String {
    let modified = metadata
        .modified()
        .ok()
        .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
        .map(|duration| duration.as_nanos())
        .unwrap_or(0);
    format!("\"{:x}-{:x}\"", modified, metadata.len())
}

/// Content types PDF.js depends on. Getting `.mjs` wrong is the single most
/// common reason the viewer or its worker fails to start.
fn content_type_for(path: &Path) -> &'static str {
    let extension = path
        .extension()
        .and_then(|extension| extension.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    match extension.as_str() {
        "html" | "htm" => "text/html; charset=utf-8",
        "js" | "mjs" => "text/javascript; charset=utf-8",
        "css" => "text/css; charset=utf-8",
        "json" | "map" => "application/json; charset=utf-8",
        "pdf" => "application/pdf",
        "wasm" => "application/wasm",
        "svg" => "image/svg+xml",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        "ico" => "image/vnd.microsoft.icon",
        "woff" => "font/woff",
        "woff2" => "font/woff2",
        "ttf" => "font/ttf",
        "otf" => "font/otf",
        // PDF.js localisation payloads, fetched as text.
        "ftl" | "properties" | "txt" => "text/plain; charset=utf-8",
        // `.bcmap` (CMap tables) and anything else is opaque binary.
        _ => "application/octet-stream",
    }
}

/// Cache policy from the recommended layout: the versioned PDF.js build is
/// immutable, the rest of the distribution is long-lived, documents are never
/// stored.
fn cache_control_for(url_path: &str) -> &'static str {
    if url_path.starts_with("/pdfjs/build/") {
        "public, max-age=31536000, immutable"
    } else if url_path.starts_with(PDFJS_PREFIX) {
        "public, max-age=31536000"
    } else {
        "no-store"
    }
}

// ---------------------------------------------------------------------------
// Path and string helpers
// ---------------------------------------------------------------------------

/// Join a URL-relative path onto a mount root, refusing anything that could
/// escape it. Traversal segments are rejected outright and the result is
/// canonicalised so a symlink cannot point outside either.
fn safe_join(root: &Path, relative: &str) -> Option<PathBuf> {
    let mut path = root.to_path_buf();
    for segment in relative.split('/') {
        if segment.is_empty() || segment == "." {
            continue;
        }
        // `..` escapes; `\` and `:` are path separators / stream separators on
        // Windows and must never come from a URL segment.
        if segment == ".." || segment.contains(['\\', ':', '\0']) {
            return None;
        }
        path.push(segment);
    }

    let mut canonical = path.canonicalize().ok()?;
    if canonical.is_dir() {
        canonical.push("index.html");
        canonical = canonical.canonicalize().ok()?;
    }
    canonical.starts_with(root).then_some(canonical)
}

fn is_loopback_host(host: &str) -> bool {
    // Strip the port, taking care with the bracketed IPv6 form.
    let name = if let Some(rest) = host.strip_prefix('[') {
        match rest.split_once(']') {
            Some((name, _)) => name,
            None => return false,
        }
    } else {
        host.split(':').next().unwrap_or("")
    };
    matches!(name, "127.0.0.1" | "localhost" | "::1")
}

fn percent_decode(input: &str) -> Option<String> {
    let bytes = input.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] == b'%' {
            if index + 2 >= bytes.len() {
                return None;
            }
            let high = (bytes[index + 1] as char).to_digit(16)?;
            let low = (bytes[index + 2] as char).to_digit(16)?;
            out.push((high * 16 + low) as u8);
            index += 3;
        } else {
            out.push(bytes[index]);
            index += 1;
        }
    }
    String::from_utf8(out).ok()
}

/// Percent-encode everything outside the unreserved set, so the result is safe
/// as a URL query-string *value* (`/` becomes `%2F`).
fn percent_encode_component(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    for &byte in input.as_bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' => {
                out.push(byte as char)
            },
            _ => out.push_str(&format!("%{byte:02X}")),
        }
    }
    out
}

fn constant_time_eq(left: &[u8], right: &[u8]) -> bool {
    if left.len() != right.len() {
        return false;
    }
    let mut difference = 0u8;
    for (a, b) in left.iter().zip(right) {
        difference |= a ^ b;
    }
    difference == 0
}

/// A 128-bit capability token, hex encoded. Errors (rather than falling back
/// to a weak source) when the OS cannot provide entropy.
fn random_token() -> io::Result<String> {
    let mut bytes = [0u8; 16];
    os_random_bytes(&mut bytes)?;
    Ok(bytes.iter().map(|byte| format!("{byte:02x}")).collect())
}

#[cfg(unix)]
fn os_random_bytes(buffer: &mut [u8]) -> io::Result<()> {
    File::open("/dev/urandom")?.read_exact(buffer)
}

#[cfg(windows)]
fn os_random_bytes(buffer: &mut [u8]) -> io::Result<()> {
    // `RtlGenRandom` is exported from advapi32 under its undecorated ordinal
    // name and is available on every supported Windows version; it needs no
    // CryptoAPI provider handle.
    #[allow(non_snake_case)]
    #[link(name = "advapi32")]
    unsafe extern "system" {
        #[link_name = "SystemFunction036"]
        fn RtlGenRandom(RandomBuffer: *mut std::ffi::c_void, RandomBufferLength: u32) -> u8;
    }
    let ok = unsafe { RtlGenRandom(buffer.as_mut_ptr().cast(), buffer.len() as u32) };
    if ok == 0 {
        return Err(io::Error::other("RtlGenRandom failed"));
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

/// Borrow a handle pointer as a reference, returning `None` on NULL.
///
/// # Safety
/// `ptr` must be NULL or a pointer from [`servo_pdf_server_start`] that has not
/// been passed to [`servo_pdf_server_stop`].
unsafe fn as_server<'a>(ptr: *mut ServoPdfServerHandle) -> Option<&'a ServoPdfServerHandle> {
    unsafe { ptr.as_ref() }
}

/// Borrow a C string argument as `&str`, treating NULL and invalid UTF-8 alike.
///
/// # Safety
/// `ptr` must be NULL or a valid NUL-terminated C string.
unsafe fn as_str<'a>(ptr: *const c_char) -> Option<&'a str> {
    if ptr.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(ptr) }.to_str().ok()
}

/// Hand a Rust string to C as a freshly allocated C string, or NULL if it
/// contains an interior NUL.
fn into_c_string(value: String) -> *mut c_char {
    match CString::new(value) {
        Ok(string) => string.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}

/// Start the loopback HTTP server.
///
/// * `pdfjs_dir` — directory holding the PDF.js distribution (the one with
///   `build/` and `web/` inside), mounted at `/pdfjs`.
/// * `documents_dir` — directory of PDFs to expose at `/documents`, or NULL to
///   mount nothing there.
///
/// Returns an owning handle, or NULL if a directory is missing, the loopback
/// socket cannot be bound, or OS entropy is unavailable. Stop it with
/// [`servo_pdf_server_stop`].
///
/// # Safety
/// Both arguments must be NULL or valid NUL-terminated C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_start(
    pdfjs_dir: *const c_char,
    documents_dir: *const c_char,
) -> *mut ServoPdfServerHandle {
    let Some(pdfjs_dir) = (unsafe { as_str(pdfjs_dir) }) else {
        return ptr::null_mut();
    };
    let documents_dir = unsafe { as_str(documents_dir) };

    match start_server(Path::new(pdfjs_dir), documents_dir.map(Path::new)) {
        Ok(server) => Box::into_raw(Box::new(server)),
        Err(_) => ptr::null_mut(),
    }
}

/// Stop the server and release the handle. Blocks until the acceptor thread
/// has exited. Passing NULL is a no-op.
///
/// # Safety
/// `server` must be NULL or a pointer from [`servo_pdf_server_start`] that has
/// not already been stopped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_stop(server: *mut ServoPdfServerHandle) {
    if server.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(server) });
}

/// The ephemeral TCP port the server is listening on, or 0 for NULL.
///
/// # Safety
/// `server` must be NULL or a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_port(server: *mut ServoPdfServerHandle) -> u16 {
    match unsafe { as_server(server) } {
        Some(server) => server.address.port(),
        None => 0,
    }
}

/// The server's origin (`http://127.0.0.1:<port>`) as a newly-allocated C
/// string, or NULL. Free with `servo_string_free()`.
///
/// # Safety
/// `server` must be NULL or a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_origin(
    server: *mut ServoPdfServerHandle,
) -> *mut c_char {
    match unsafe { as_server(server) } {
        Some(server) => into_c_string(server.origin.clone()),
        None => ptr::null_mut(),
    }
}

/// Absolute URL for a server-relative `path` such as `/pdfjs/web/viewer.html`
/// or `/documents/report.pdf`, with the capability token spliced in.
///
/// Returns NULL if `path` does not start with `/`. Free with
/// `servo_string_free()`.
///
/// # Safety
/// `server` must be NULL or a valid handle; `path` a valid C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_url(
    server: *mut ServoPdfServerHandle,
    path: *const c_char,
) -> *mut c_char {
    let Some(server) = (unsafe { as_server(server) }) else {
        return ptr::null_mut();
    };
    let Some(path) = (unsafe { as_str(path) }) else {
        return ptr::null_mut();
    };
    if !path.starts_with('/') {
        return ptr::null_mut();
    }
    into_c_string(server.url_for(path))
}

/// Build the PDF.js viewer URL for a document — the URL to hand to
/// `servo_webview_load_uri()`.
///
/// * `document_path` — either a path relative to the document mount
///   (`report.pdf`, `invoices/2026-01.pdf`) or a server-relative path starting
///   with `/`.
/// * `viewer_hash` — optional PDF.js hash parameters without the leading `#`,
///   e.g. `page=3&zoom=page-width&pagemode=none`. May be NULL.
///
/// The `file` parameter is emitted same-origin and percent-encoded, which is
/// what the viewer's origin check requires. Free with `servo_string_free()`.
///
/// # Safety
/// `server` must be NULL or a valid handle; the strings valid C strings or NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_viewer_url(
    server: *mut ServoPdfServerHandle,
    document_path: *const c_char,
    viewer_hash: *const c_char,
) -> *mut c_char {
    let Some(server) = (unsafe { as_server(server) }) else {
        return ptr::null_mut();
    };
    let Some(document_path) = (unsafe { as_str(document_path) }) else {
        return ptr::null_mut();
    };
    let hash = unsafe { as_str(viewer_hash) };

    let file_path = if document_path.starts_with('/') {
        document_path.to_owned()
    } else {
        format!("{DOCUMENTS_PREFIX}{document_path}")
    };
    // Same-origin, token-scoped, and encoded as one opaque query value.
    let file = percent_encode_component(&format!("/{}{}", server.config.token, file_path));

    let mut url = format!("{}?file={file}", server.url_for(DEFAULT_VIEWER_PATH));
    if let Some(hash) = hash.filter(|hash| !hash.is_empty()) {
        url.push('#');
        url.push_str(hash);
    }
    into_c_string(url)
}

/// Publish a single file at `/documents/<name>`.
///
/// This is the counterpart to the `documents_dir` mount for the common case of
/// "the user picked this one file": nothing but the named file becomes
/// reachable, and it is served from the canonical path resolved here rather
/// than from anything in the URL.
///
/// * `file_path` — the file to publish; must exist and be a regular file.
/// * `name` — the name to publish it under, or NULL to use the file's own base
///   name. Must be a single path segment.
///
/// Publishing over an existing name replaces it. Returns the server-relative
/// path (`/documents/<name>`), ready to hand to
/// [`servo_pdf_server_viewer_url`], or NULL on failure. Free with
/// `servo_string_free()`.
///
/// # Safety
/// `server` must be NULL or a valid handle; the strings valid C strings or NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_add_document(
    server: *mut ServoPdfServerHandle,
    file_path: *const c_char,
    name: *const c_char,
) -> *mut c_char {
    let Some(server) = (unsafe { as_server(server) }) else {
        return ptr::null_mut();
    };
    let Some(file_path) = (unsafe { as_str(file_path) }) else {
        return ptr::null_mut();
    };

    let Ok(canonical) = Path::new(file_path).canonicalize() else {
        return ptr::null_mut();
    };
    if !canonical.is_file() {
        return ptr::null_mut();
    }

    let name = match unsafe { as_str(name) } {
        Some(name) => name.to_owned(),
        None => match canonical.file_name().and_then(|name| name.to_str()) {
            Some(name) => name.to_owned(),
            None => return ptr::null_mut(),
        },
    };
    if !is_publishable_name(&name) {
        return ptr::null_mut();
    }

    let Ok(mut documents) = server.config.documents.write() else {
        return ptr::null_mut();
    };
    documents.insert(name.clone(), canonical);
    into_c_string(format!("{DOCUMENTS_PREFIX}{name}"))
}

/// Withdraw a document published by [`servo_pdf_server_add_document`].
///
/// `name` may be either the bare name or the `/documents/<name>` path that
/// call returned. Returns true if something was withdrawn.
///
/// # Safety
/// `server` must be NULL or a valid handle; `name` a valid C string or NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_remove_document(
    server: *mut ServoPdfServerHandle,
    name: *const c_char,
) -> bool {
    let Some(server) = (unsafe { as_server(server) }) else {
        return false;
    };
    let Some(name) = (unsafe { as_str(name) }) else {
        return false;
    };
    let name = name.strip_prefix(DOCUMENTS_PREFIX).unwrap_or(name);

    match server.config.documents.write() {
        Ok(mut documents) => documents.remove(name).is_some(),
        Err(_) => false,
    }
}

/// Add `'unsafe-inline'` to the `style-src` of PDF.js's own `<meta>` CSP.
///
/// **This relaxes a security control, so it is off by default.** It exists
/// because of a Servo performance bug: with any CSP that restricts inline
/// styles, Servo re-runs a full CSP evaluation for the UA shadow content of
/// `<input type="range">` on every restyle. PDF.js's viewer has five of them,
/// which is enough to saturate the script thread — the viewer chrome paints
/// but pages take minutes to appear, or never do.
///
/// Enabling this only widens an existing `style-src`; no other directive is
/// touched, and the server's own CSP response header still applies. The
/// exposure it adds is that a stylesheet injected into the viewer page would no
/// longer be blocked — unattractive, but bounded on a loopback origin serving
/// only files you published. Leave it off unless you have measured the stall.
///
/// # Safety
/// `server` must be NULL or a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_set_relax_style_csp(
    server: *mut ServoPdfServerHandle,
    relax: bool,
) {
    if let Some(server) = unsafe { as_server(server) } {
        server.config.relax_style_csp.store(relax, Ordering::Relaxed);
    }
}

/// Seed PDF.js viewer preferences for every page served from `/pdfjs`.
///
/// `preferences_json` is a JSON **object** of PDF.js preference names to
/// values, e.g.
/// `{"enableScripting":false,"sidebarViewOnLoad":0,"defaultZoomValue":"page-width"}`.
/// The server injects a small bootstrap script into the viewer HTML that merges
/// them into the viewer's stored preferences before it boots, so the
/// distribution's own files stay untouched. Pass NULL to stop injecting.
///
/// Returns `true` if the preferences were accepted (or cleared), `false` if the
/// JSON is invalid or is not an object — in which case nothing changes.
///
/// # Safety
/// `server` must be NULL or a valid handle; `preferences_json` a valid C string
/// or NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn servo_pdf_server_set_viewer_preferences(
    server: *mut ServoPdfServerHandle,
    preferences_json: *const c_char,
) -> bool {
    let Some(server) = (unsafe { as_server(server) }) else {
        return false;
    };

    let value = match unsafe { as_str(preferences_json) } {
        None => None,
        Some(json) => {
            // Re-serialise through serde_json so only well-formed JSON — and
            // only an object — is ever embedded in the bootstrap script.
            let Ok(parsed) = serde_json::from_str::<serde_json::Value>(json) else {
                return false;
            };
            if !parsed.is_object() {
                return false;
            }
            Some(parsed.to_string())
        },
    };

    match server.config.viewer_preferences.write() {
        Ok(mut guard) => {
            *guard = value;
            true
        },
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn range_parsing_covers_the_forms_pdfjs_uses() {
        assert!(matches!(
            parse_range("bytes=0-65535", 1_000_000),
            RangeSpec::Partial {
                start: 0,
                end: 65535
            }
        ));
        assert!(matches!(
            parse_range("bytes=900-", 1000),
            RangeSpec::Partial {
                start: 900,
                end: 999
            }
        ));
        assert!(matches!(
            parse_range("bytes=-100", 1000),
            RangeSpec::Partial {
                start: 900,
                end: 999
            }
        ));
        // Clamped to the entity, not rejected.
        assert!(matches!(
            parse_range("bytes=0-99999", 10),
            RangeSpec::Partial { start: 0, end: 9 }
        ));
        assert!(matches!(
            parse_range("bytes=10-", 10),
            RangeSpec::Unsatisfiable
        ));
        assert!(matches!(parse_range("bytes=-0", 10), RangeSpec::Unsatisfiable));
        // Multi-range and junk both degrade to the whole entity.
        assert!(matches!(parse_range("bytes=0-1,5-6", 10), RangeSpec::Whole));
        assert!(matches!(parse_range("items=0-1", 10), RangeSpec::Whole));
    }

    #[test]
    fn module_scripts_are_served_as_javascript() {
        assert_eq!(
            content_type_for(Path::new("build/pdf.worker.mjs")),
            "text/javascript; charset=utf-8"
        );
        assert_eq!(content_type_for(Path::new("doc.pdf")), "application/pdf");
        assert_eq!(
            content_type_for(Path::new("web/cmaps/Adobe-Japan1-0.bcmap")),
            "application/octet-stream"
        );
    }

    #[test]
    fn cache_policy_splits_build_assets_from_documents() {
        assert_eq!(
            cache_control_for("/pdfjs/build/pdf.mjs"),
            "public, max-age=31536000, immutable"
        );
        assert_eq!(
            cache_control_for("/pdfjs/web/viewer.html"),
            "public, max-age=31536000"
        );
        assert_eq!(cache_control_for("/documents/a.pdf"), "no-store");
    }

    #[test]
    fn traversal_and_absolute_segments_are_rejected() {
        let root = std::env::temp_dir().canonicalize().expect("temp dir");
        assert!(safe_join(&root, "../etc/passwd").is_none());
        assert!(safe_join(&root, "a/../../etc/passwd").is_none());
        assert!(safe_join(&root, "C:windows").is_none());
        assert!(safe_join(&root, "a\\..\\b").is_none());
        // Nonexistent paths simply do not resolve.
        assert!(safe_join(&root, "definitely/not/here.pdf").is_none());
    }

    #[test]
    fn only_loopback_hosts_are_accepted() {
        assert!(is_loopback_host("127.0.0.1:41235"));
        assert!(is_loopback_host("localhost:41235"));
        assert!(is_loopback_host("[::1]:41235"));
        assert!(!is_loopback_host("example.com:41235"));
        assert!(!is_loopback_host("127.0.0.1.evil.test"));
    }

    #[test]
    fn percent_coding_round_trips_paths() {
        assert_eq!(percent_decode("/a%20b/c.pdf").as_deref(), Some("/a b/c.pdf"));
        assert_eq!(percent_decode("/a%2"), None);
        assert_eq!(percent_decode("/a%zz"), None);
        assert_eq!(percent_encode_component("/docs/a b.pdf"), "%2Fdocs%2Fa%20b.pdf");
    }

    #[test]
    fn script_is_injected_before_the_head_closes() {
        let html = "<!DOCTYPE html><html><HEAD><title>x</title></HEAD><body></body></html>";
        let out = inject_script(html, "/tok/pdfjs/__servo_embed_prefs.js");
        let script = out.find("<script").expect("script injected");
        let head = out.to_ascii_lowercase().find("</head>").expect("head");
        assert!(script < head);
    }

    /// Lay out a miniature PDF.js distribution plus one document, and return
    /// the temporary root (removed by the caller).
    fn fixture(name: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!("servo-pdf-server-{name}"));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("pdfjs/build")).expect("build dir");
        std::fs::create_dir_all(root.join("pdfjs/web")).expect("web dir");
        std::fs::create_dir_all(root.join("documents")).expect("documents dir");
        std::fs::write(
            root.join("pdfjs/web/viewer.html"),
            "<html><head><title>viewer</title></head><body></body></html>",
        )
        .expect("viewer.html");
        std::fs::write(root.join("pdfjs/build/pdf.worker.mjs"), "export const x = 1;\n")
            .expect("worker");
        std::fs::write(root.join("documents/a.pdf"), b"%PDF-1.7\n0123456789")
            .expect("document");
        root
    }

    /// Send one raw request and split the reply into head and body.
    fn request(address: SocketAddr, raw: &str) -> (String, Vec<u8>) {
        let mut stream = TcpStream::connect(address).expect("connect");
        stream.write_all(raw.as_bytes()).expect("write request");
        stream.flush().expect("flush");
        let mut reply = Vec::new();
        stream.read_to_end(&mut reply).expect("read reply");
        let split = reply
            .windows(4)
            .position(|window| window == b"\r\n\r\n")
            .expect("headers terminated");
        (
            String::from_utf8_lossy(&reply[..split]).into_owned(),
            reply[split + 4..].to_vec(),
        )
    }

    fn get(address: SocketAddr, path: &str) -> (String, Vec<u8>) {
        request(
            address,
            &format!(
                "GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
            ),
        )
    }

    #[test]
    fn server_serves_pdfjs_over_loopback() {
        let root = fixture("serve");
        let server = start_server(&root.join("pdfjs"), Some(&root.join("documents")))
            .expect("server starts");
        let address = server.address;
        let token = server.config.token.clone();

        // The capability token is mandatory, and its absence looks like a 404.
        let (head, _) = get(address, "/pdfjs/web/viewer.html");
        assert!(head.starts_with("HTTP/1.1 404 "), "{head}");
        let (head, _) = get(address, "/0123456789abcdef0123456789abcdef/pdfjs/web/viewer.html");
        assert!(head.starts_with("HTTP/1.1 404 "), "{head}");

        // Module scripts must arrive as JavaScript, cached immutably.
        let (head, body) = get(address, &format!("/{token}/pdfjs/build/pdf.worker.mjs"));
        assert!(head.starts_with("HTTP/1.1 200 "), "{head}");
        assert!(head.contains("Content-Type: text/javascript; charset=utf-8"), "{head}");
        assert!(head.contains("Cache-Control: public, max-age=31536000, immutable"), "{head}");
        assert!(head.contains("Accept-Ranges: bytes"), "{head}");
        assert_eq!(body, b"export const x = 1;\n");

        // HTML carries the viewer CSP.
        let (head, _) = get(address, &format!("/{token}/pdfjs/web/viewer.html"));
        assert!(head.contains("Content-Security-Policy: default-src 'self';"), "{head}");
        assert!(head.contains("worker-src 'self' blob:"), "{head}");

        // Documents are never stored, and range requests are honoured.
        let (head, body) = get(address, &format!("/{token}/documents/a.pdf"));
        assert!(head.contains("Content-Type: application/pdf"), "{head}");
        assert!(head.contains("Cache-Control: no-store"), "{head}");
        assert_eq!(body.len(), 19);

        let (head, body) = request(
            address,
            &format!(
                "GET /{token}/documents/a.pdf HTTP/1.1\r\nHost: 127.0.0.1\r\n\
                 Range: bytes=9-13\r\nConnection: close\r\n\r\n"
            ),
        );
        assert!(head.starts_with("HTTP/1.1 206 "), "{head}");
        assert!(head.contains("Content-Range: bytes 9-13/19"), "{head}");
        assert_eq!(body, b"01234");

        // HEAD reports the length without the body.
        let (head, body) = request(
            address,
            &format!(
                "HEAD /{token}/documents/a.pdf HTTP/1.1\r\nHost: 127.0.0.1\r\n\
                 Connection: close\r\n\r\n"
            ),
        );
        assert!(head.contains("Content-Length: 19"), "{head}");
        assert!(body.is_empty());

        // Traversal out of a mount, and the unmounted parts of the tree, 404.
        let (head, _) = get(address, &format!("/{token}/documents/..%2Fpdfjs/build/pdf.worker.mjs"));
        assert!(head.starts_with("HTTP/1.1 404 "), "{head}");
        let (head, _) = get(address, &format!("/{token}/etc/passwd"));
        assert!(head.starts_with("HTTP/1.1 404 "), "{head}");

        // A forged (non-loopback) Host is a rebinding attempt.
        let (head, _) = request(
            address,
            &format!(
                "GET /{token}/documents/a.pdf HTTP/1.1\r\nHost: evil.test\r\n\
                 Connection: close\r\n\r\n"
            ),
        );
        assert!(head.starts_with("HTTP/1.1 421 "), "{head}");

        // Anything that could carry a body would desynchronise the connection.
        let (head, _) = request(
            address,
            &format!(
                "POST /{token}/documents/a.pdf HTTP/1.1\r\nHost: 127.0.0.1\r\n\
                 Connection: close\r\n\r\n"
            ),
        );
        assert!(head.starts_with("HTTP/1.1 405 "), "{head}");

        drop(server);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn published_documents_expose_one_file_each() {
        let root = fixture("publish");
        // No directory mount at all: only what we publish is reachable.
        let server = start_server(&root.join("pdfjs"), None).expect("server starts");
        let address = server.address;
        let token = server.config.token.clone();
        let handle = &server as *const ServoPdfServerHandle as *mut ServoPdfServerHandle;

        let (head, _) = get(address, &format!("/{token}/documents/a.pdf"));
        assert!(head.starts_with("HTTP/1.1 404 "), "{head}");

        let document = root.join("documents/a.pdf");
        let document = CString::new(document.to_str().unwrap()).unwrap();
        let published = unsafe {
            servo_pdf_server_add_document(handle, document.as_ptr(), ptr::null())
        };
        let published = unsafe { CString::from_raw(published) }
            .into_string()
            .expect("utf-8");
        assert_eq!(published, "/documents/a.pdf");

        let (head, body) = get(address, &format!("/{token}{published}"));
        assert!(head.starts_with("HTTP/1.1 200 "), "{head}");
        assert!(head.contains("Content-Type: application/pdf"), "{head}");
        assert_eq!(body.len(), 19);

        // Its neighbours in the same directory stay invisible.
        let (head, _) = get(address, &format!("/{token}/documents/b.pdf"));
        assert!(head.starts_with("HTTP/1.1 404 "), "{head}");

        // A published name is one segment, so it cannot shadow a subtree.
        let (head, _) = get(address, &format!("/{token}/documents/a.pdf/x"));
        assert!(head.starts_with("HTTP/1.1 404 "), "{head}");

        assert!(unsafe { servo_pdf_server_remove_document(handle, c"/documents/a.pdf".as_ptr()) });
        assert!(!unsafe { servo_pdf_server_remove_document(handle, c"a.pdf".as_ptr()) });
        let (head, _) = get(address, &format!("/{token}/documents/a.pdf"));
        assert!(head.starts_with("HTTP/1.1 404 "), "{head}");

        // Missing files and unusable names are refused.
        assert!(
            unsafe { servo_pdf_server_add_document(handle, c"/nope/nope.pdf".as_ptr(), ptr::null()) }
                .is_null()
        );
        assert!(
            unsafe { servo_pdf_server_add_document(handle, document.as_ptr(), c"..".as_ptr()) }
                .is_null()
        );
        assert!(
            unsafe { servo_pdf_server_add_document(handle, document.as_ptr(), c"a/b".as_ptr()) }
                .is_null()
        );

        drop(server);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn pdfjs_html_always_carries_the_engine_shims() {
        let root = fixture("prefs");
        let server = start_server(&root.join("pdfjs"), None).expect("server starts");
        let address = server.address;
        let token = server.config.token.clone();
        let handle = &server as *const ServoPdfServerHandle as *mut ServoPdfServerHandle;

        // Even with no preferences set, the shims must be there: without them
        // PDF.js 6.x throws during initialize() and renders a blank viewer.
        let (head, body) = get(address, &format!("/{token}/pdfjs/web/viewer.html"));
        let body = String::from_utf8_lossy(&body);
        assert!(head.contains("Cache-Control: no-store"), "{head}");
        let script = body.find("__servo_embed_bootstrap.js").expect("tag injected");
        assert!(script < body.find("</head>").expect("head"));

        let (head, boot) = get(address, &format!("/{token}/pdfjs/__servo_embed_bootstrap.js"));
        let boot = String::from_utf8_lossy(&boot);
        assert!(head.contains("Content-Type: text/javascript; charset=utf-8"), "{head}");
        assert!(boot.contains("getOrInsertComputed"), "shim missing");
        assert!(boot.contains("unhandledrejection"), "rejection reporter missing");
        // Auto-linking is off by default even with no embedder preferences:
        // Servo ignores the clip-path PDF.js uses to size inferred links.
        assert!(boot.contains("\"enableAutoLinking\":false"), "default missing");

        // Only JSON objects are accepted.
        assert!(!unsafe {
            servo_pdf_server_set_viewer_preferences(handle, c"[1,2]".as_ptr())
        });
        assert!(!unsafe {
            servo_pdf_server_set_viewer_preferences(handle, c"{oops}".as_ptr())
        });
        assert!(unsafe {
            servo_pdf_server_set_viewer_preferences(
                handle,
                c"{\"enableScripting\":false}".as_ptr(),
            )
        });

        let (_, boot) = get(address, &format!("/{token}/pdfjs/__servo_embed_bootstrap.js"));
        let boot = String::from_utf8_lossy(&boot);
        assert!(boot.contains("getOrInsertComputed"), "shim lost");
        assert!(boot.contains("\"enableScripting\":false"));
        assert!(boot.contains("webviewerloaded"));

        drop(server);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn servo_defaults_apply_but_the_embedder_wins() {
        let root = fixture("defaults");
        let server = start_server(&root.join("pdfjs"), None).expect("server starts");
        let address = server.address;
        let token = server.config.token.clone();
        let handle = &server as *const ServoPdfServerHandle as *mut ServoPdfServerHandle;

        // An unrelated preference must not displace the defaults.
        assert!(unsafe {
            servo_pdf_server_set_viewer_preferences(handle, c"{\"enableScripting\":false}".as_ptr())
        });
        let (_, boot) = get(address, &format!("/{token}/pdfjs/__servo_embed_bootstrap.js"));
        let boot = String::from_utf8_lossy(&boot);
        assert!(boot.contains("\"enableAutoLinking\":false"), "default lost: {boot}");
        assert!(boot.contains("\"enableScripting\":false"));

        // Setting it explicitly overrides the default.
        assert!(unsafe {
            servo_pdf_server_set_viewer_preferences(handle, c"{\"enableAutoLinking\":true}".as_ptr())
        });
        let (_, boot) = get(address, &format!("/{token}/pdfjs/__servo_embed_bootstrap.js"));
        let boot = String::from_utf8_lossy(&boot);
        assert!(boot.contains("\"enableAutoLinking\":true"), "override ignored: {boot}");
        assert!(!boot.contains("\"enableAutoLinking\":false"));

        drop(server);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn mask_icons_gain_a_background_image_fallback() {
        // The standard property is augmented; the -webkit- alias is left alone
        // so the icon is not declared twice.
        let css = "#zoomIn::before{-webkit-mask-image:var(--i);mask-image:var(--i);}";
        let out = inline_mask_icon_fallback(css);
        assert_eq!(out.matches("background-image:var(--i)").count(), 1, "{out}");
        assert!(out.contains("-webkit-mask-image:var(--i);"), "{out}");
        assert!(out.contains("background-color:transparent"), "{out}");
        // The original declaration survives, so a masking engine still masks.
        assert!(out.contains("mask-image:var(--i);background-image:"), "{out}");

        // A declaration ending at the closing brace rather than a semicolon.
        let out = inline_mask_icon_fallback("a{mask-image:url(x.svg)}");
        assert!(out.contains("background-image:url(x.svg)"), "{out}");
        assert!(out.ends_with("contain}"), "{out}");

        // Sheets without masking are returned untouched.
        let plain = "a{color:red}";
        assert_eq!(inline_mask_icon_fallback(plain), plain);
    }

    #[test]
    fn style_csp_relaxation_only_widens_style_src() {
        // Off by default: the document's own policy is served verbatim.
        let csp = "default-src 'none'; style-src 'self'; script-src 'self'";
        assert_eq!(
            relax_style_csp(csp),
            "default-src 'none'; style-src 'self' 'unsafe-inline'; script-src 'self'"
        );
        // Idempotent, and blind to documents that never restricted styles.
        assert_eq!(
            relax_style_csp("style-src 'self' 'unsafe-inline'"),
            "style-src 'self' 'unsafe-inline'"
        );
        assert_eq!(relax_style_csp("default-src 'none'"), "default-src 'none'");
        // Stops at the attribute quote, so surrounding markup is preserved.
        assert_eq!(
            relax_style_csp("<meta content=\"style-src 'self'\" />"),
            "<meta content=\"style-src 'self' 'unsafe-inline'\" />"
        );
    }

    #[test]
    fn viewer_url_is_same_origin_and_token_scoped() {
        let root = fixture("url");
        let server = start_server(&root.join("pdfjs"), Some(&root.join("documents")))
            .expect("server starts");
        let token = server.config.token.clone();

        let handle = &server as *const ServoPdfServerHandle as *mut ServoPdfServerHandle;
        let url = unsafe { servo_pdf_server_viewer_url(handle, c"a.pdf".as_ptr(), c"zoom=page-width".as_ptr()) };
        let owned = unsafe { CString::from_raw(url) }.into_string().expect("utf-8");
        assert_eq!(
            owned,
            format!(
                "http://127.0.0.1:{}/{token}/pdfjs/web/viewer.html?file=%2F{token}%2Fdocuments%2Fa.pdf#zoom=page-width",
                server.address.port()
            )
        );

        drop(server);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn dates_are_imf_fixdate() {
        let at = |seconds| http_date(UNIX_EPOCH + Duration::from_secs(seconds));
        assert_eq!(at(0), "Thu, 01 Jan 1970 00:00:00 GMT");
        assert_eq!(at(784_111_777), "Sun, 06 Nov 1994 08:49:37 GMT");
        // Leap day, and the turn of a non-leap century.
        assert_eq!(at(951_782_400), "Tue, 29 Feb 2000 00:00:00 GMT");
        assert_eq!(at(4_107_542_400), "Mon, 01 Mar 2100 00:00:00 GMT");
    }

    #[test]
    fn tokens_are_distinct_and_hex() {
        let first = random_token().expect("entropy");
        let second = random_token().expect("entropy");
        assert_eq!(first.len(), 32);
        assert!(first.bytes().all(|byte| byte.is_ascii_hexdigit()));
        assert_ne!(first, second);
    }
}
