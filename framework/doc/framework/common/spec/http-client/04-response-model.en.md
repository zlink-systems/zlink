# 4. Response Contract

> [Common contract table of contents](README.en.md)

## 4.1 Terminator Axis (§5.1)

| Form | Terminator (names in [Language interfaces §1.4](language-interfaces.en.md#14-terminators)) | Returns | Failure Condition |
| --- | --- | --- | --- |
| raw | raw response terminator | `RawHttpResponse` | Only transport failure. **Status isn't failure** (4xx/5xx also return success) |
| typed | typed response terminator | `HttpResponse<T>` | Transport failure + **status ≥ 400** + decode failure |
| download | download terminator | `RawHttpResponse` (empty body) | Transport failure. Status is treated the same as raw |

- A typed submit's status ≥ 400 is reported as `InternalFailure`. Under
  the current contract, the response body isn't exposed in this case —
  if the error payload is needed, use the raw response terminator (revision candidate
  [R1](10-revision-candidates.en.md)).
- Whether a blocking terminator exists, and under what name, is decided by
  [Language interfaces §1.4](language-interfaces.en.md#14-terminators). If only
  a typed response's body is needed, use the body-only terminator (`fetch`).

## 4.2 Response Types

- `RawHttpResponse { status: int, headers: map<string,string>, body: string }`
- `HttpResponse<T> { status: int, headers, body: T, rawBody: string }`
- A HEAD response and 204 have an empty body. In the typed path, an
  empty success response's body is the language's "none" value (such
  as null).

## 4.3 Header Representation

- Response header name lookup must be **equivalent to case-insensitive**.
  cpp/java/node use a lowercase-normalized map, and dotnet preserves
  original casing + a case-insensitive map (`OrdinalIgnoreCase`) — both
  satisfy the contract (cpp's case-sensitive map deviation was
  resolved by the 2026-07-12 R6 promotion).

## 4.4 `download(sink)` Semantics

- The sink is a push-style chunk callback (byte view: `string_view` /
  `ReadOnlyMemory<byte>` / `byte[]` / `ByteArray` / `Uint8Array`).
- The `maxResponseBodySize` bound applies to the accumulated size.
- **Decompression isn't applied** — the raw bytes are delivered as is
  ([Chapter 8](08-compression.en.md)).
- Excluded from retry (the sink can't be replayed).
- During redirect following, **an intermediate response's body doesn't
  leak into the sink**. Only the final response flows to the sink.

## 4.5 Typed Decode

- A JSON decode failure is `ProtocolError`.
- node removes the `__proto__`/`constructor`/`prototype` keys as a
  prototype-pollution defense (a language-specific security rule, not a
  contract violation).
