## General

* Respond in English unless the code or existing comments are in another language.
* Assume gcc on linux, cmake, Cargo.
* Cross platform Mingw
* Framework is Gtk3 and Gtk4
* We are make a wrapper for Servo Web View written in Rust.
* Make small, safe changes rather than large refactorings.
* Preserve the existing architecture, naming conventions, and formatting.

## Rust
* Use the existing Rust code as a reference.
* Use the existing Rust code as a guide.
* Use the existing Rust code as a starting point.
* Use the existing Rust code as a template.
* Use 'tls-model=global-dynamic'

## Embedding API
* Use 'disable_initial_exec_tls' for embedding API

## Security

* Do not suggest code that weakens TLS, authentication, certificate validation, or access control.
* Fail closed on authentication, certificate, and validation failures.
* Clearly mark any insecure suggestions.

## Testing

* Suggest unit tests for modified business logic.
* Use xUnit or the existing test framework in the project.
* Test success, failure, and edge cases.

## Git

* Suggest small commits with clear commit messages.
* Avoid mixing formatting-only changes with logic changes in the same commit.
