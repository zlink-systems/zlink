[English](./framework-cpp-0.19.0.md) | [한국어](./framework-cpp-0.19.0.ko.md)

# ZLink C++ Framework 0.19.0 Release Notes

Framework 0.19.0 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

- HTTP client (C++): the terminators follow the binding policy (async stem `async`, blocking stem `submit`). Async: **`async_raw()`, `async<T>()`, `async<T>(callback)`, `fetch<T>()`** (all `task_t`, run on the execute scheduler with or without `.coroutines(...)`). Blocking: **`submit_raw()`, `submit<T>()`** (return `result_t`; `invalid_operation` in a runtime execution context). The former task-returning `submit<T>()`, blocking `fetch<T>()` and the server builder one-way `submit()` are gone. (#707)
- HTTP client: HTTP has no request without a response, so the **one-way terminator is removed**. A call that does not need the response value leaves the raw response terminator's result unused. Terminator names are owned by the binding policy and Framework Submit and completion §2; HTTP language interfaces §1.4 maps response shapes onto them. (#707)
- HTTP client: error kinds now follow spec chapter 09 §9.1. Response body over the limit is `Rejected`, redirect format and limit failures are `ProtocolError`, transport, connection-refused, proxy and TLS failures are `Unavailable`, timeouts are `DeadlineExceeded`. Callers that branch on the kind are affected. (#704)

## Common Changes

- The tutorial gains an `HttpClient` program: eleven steps that call the tutorial Client's HTTP surface with the HTTP client. The Client and Server HTTP surfaces gain admin Basic auth, gzip on `GET /rooms/{id}`, a 301 on `GET /player/{id}`, a chunked download on `GET /rooms/{id}/export` and a streaming upload on `POST /rooms/{id}/import`. (#705)
- The HTTP client user guide is rewritten as eleven chapters generated from a common source shared by the five languages (language tabs). Feature-chapter code is read as snippets from the tutorial `HttpClient`. (#706)

## Install

Download `zlink-framework-cpp-0.19.0.tar.gz` from the [`framework-cpp/v0.19.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.19.0) and use `find_package(zlink_framework CONFIG REQUIRED)`. The vcpkg overlay port and the Conan recipe also install it.

The release tag is [`framework-cpp/v0.19.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.19.0).
