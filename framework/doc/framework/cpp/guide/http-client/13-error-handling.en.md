[← Table Of Contents](README.en.md)

# 13. Error Handling

Every failure is reported through the common `zlink::framework` error model. The form you receive it
in is one of two, depending on the consumption method.

- `result_t` — for `.result()` or a callback submit. Branch with `operator bool`, access detail with
  `error()`.
- A `framework_exception_t` exception — for `co_await` and `fetch<T>()`. It has `kind()`, `what()`,
  `is_retriable()`.

The same failure is just expressed in two forms — the classification is identical.

## Error Kind Mapping

| kind | When | retriable |
|------|------|-----------|
| `request_protocol_error` | Invalid configuration/input: bad base_url/scheme, timeout 0 or below, 0-byte response body cap, empty header name, path not starting with `/`, multiple body sources, `nullptr` coroutine scheduler, https on a build with no OpenSSL | ✗ |
| `request_failed` | Transport failure (connection refused/dropped, TLS verification failure), 4xx/5xx on the typed path, redirect limit exceeded, response body cap exceeded, proxy CONNECT rejected | transport is ✓, the rest ✗ |
| `timeout` | Client/request timeout exceeded. On a coroutine client, the timeout is computed from the moment it's registered with the scheduler queue | ✓ |
| `payload_decode_failed` | Response JSON decode failure, corrupted gzip/deflate body | ✗ |
| `closed` | An uninitialized client (used after default-constructing `client_t{}`), a custom execute scheduler rejecting work registration | ✗ |

A configuration error (`request_protocol_error`) is deliberately distinguished from a transport
failure — because it's a code bug, so retrying is meaningless. It's often thrown right away, at
setter/`build()` time.

## The result_t Pattern

```cpp
auto result = client.get ("/players/7281").submit<player_profile_t> ().result ();

if (!result) {
    const auto *error = result.error ();
    switch (error->kind ()) {
        case zlink::framework::framework_error_kind_t::timeout:
            metrics.count ("player_lookup.timeout");
            break;
        case zlink::framework::framework_error_kind_t::payload_decode_failed:
            log_error ("schema mismatch: {}", error->what ());
            break;
        default:
            log_error ("player lookup failed: {}", error->what ());
    }
    return std::nullopt;
}
return result.value ().body;
```

## The Exception Pattern (co_await / fetch)

```cpp
try {
    auto profile = client.get ("/players/7281").fetch<player_profile_t> ();
    render (profile);
}
catch (const zlink::framework::framework_exception_t &error) {
    if (error.is_retriable ()) {
        schedule_retry ();
    } else {
        report_permanent_failure (error.what ());
    }
}
```

## Which Side Is 4xx/5xx On

- `submit<T>()`/`fetch<T>()` (typed): **a failure** — `request_failed`,
  "HTTP request failed with status 404".
- `submit_raw()`: **a success** — you branch on the status directly
  ([6. Handling Responses](06-handling-responses.en.md)).

If the business logic cares about a status like 404/409, use the raw path; if "200 + DTO, otherwise
a failure" is right, use the typed path.

## The Relationship Between is_retriable And Automatic Retry

The scope `retry(attempts)` ([Chapter 10](10-redirects-retries-cookies.en.md)) automatically retries
is exactly the failures where `is_retriable() == true`. Using the same criterion when writing your
own retry loop keeps it consistent.

```cpp
for (int attempt = 0;; ++attempt) {
    auto result = client.get ("/ready").submit_raw ().result ();
    if (result || attempt >= 3 || !result.error ()->is_retriable ()) {
        return result;
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (200 << attempt));
}
```

[← Table Of Contents](README.en.md)
