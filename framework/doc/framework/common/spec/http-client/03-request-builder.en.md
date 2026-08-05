# 3. Request Contract

> [Common contract table of contents](README.en.md)

## 3.1 HTTP Method

7 kinds: `get / post / put / delete / patch / head / options`. Provided
with the same name on both the client instance and the client builder
(one-shot).

Language deviation: cpp uses `delete_` for keyword avoidance.

## 3.2 Path And Query

- Path must start with `/`. Otherwise `ProtocolError`.
- `query(name, value)` applies percent-encoding and accumulates it into
  the URL.
- The baseUrl + path combination and query serialization result must be
  identical across the 5 languages (a contract test axis).

## 3.3 Header

- `header(name, value)` — a per-request header. **Takes priority over
  the client's `defaultHeader`.**
- Header name is compared case-insensitively.
- The wrapper auto-injects the following, which the user can override:
  `user-agent: zlink-http-client/<version>`, `accept: application/json`.

## 3.4 Per-Request Timeout

`timeout(time)` — overrides the client default timeout only for this
request. The semantics are the same as the per-attempt timeout in
[Chapter 6 §6.2](06-redirect-retry-cookie.en.md).

## 3.5 The 5 Body Sources And The Exclusion Rule

| Source | Signature (Concept) | content-type | retry |
| --- | --- | --- | --- |
| typed JSON | `body(dto)` | `application/json` automatic (not overwritten if explicit) | possible |
| raw | `body(content, contentType)` | as given by the argument | possible |
| streaming upload | `bodyStream(provider, contentType)` | as given by the argument, chunked transfer | **excluded** |
| form | `form(name, value)` accumulates | `application/x-www-form-urlencoded` | possible |
| multipart | `multipart(name, value)` / `multipartFile(name, filename, content, contentType)` accumulates | `multipart/form-data` + boundary | possible |

- **Mutually exclusive**: mixing different sources in one request is
  `ProtocolError` ("single body source").
- Typed JSON serialization is delegated to the language codec layer:
  cpp `to_json` (nlohmann ADL), dotnet codec registry (default
  `System.Text.Json` Web), java/kotlin Jackson, node
  `JSON.stringify`.
- A streaming provider is pull-based: it returns a chunk, and signals
  end with the language-idiomatic "none" value (cpp `std::nullopt`,
  dotnet/java/kotlin/node `null`).
- Since streaming upload can't rewind, it's excluded from retry and
  redirect resubmission ([Chapter 6](06-redirect-retry-cookie.en.md)).
- `multipartFile`'s content is a string under the current contract.
  Binary file upload is routed around with `bodyStream` (revision
  candidate [R4](10-revision-candidates.en.md)).

## 3.6 Example (Conceptual Notation)

```
client.post("/games")
      .header("x-request-id", "req-8f2c41")
      .query("region", "kr")
      .body(create_game_req)     // typed JSON
      .timeout(3s)
      .submit<create_game_res>() // C++/Java's typed response terminator
                                 // Node uses async<create_game_res>()
```
