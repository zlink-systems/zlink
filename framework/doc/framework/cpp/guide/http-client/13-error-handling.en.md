[← Table Of Contents](README.en.md)

# 13. Error Handling

Every failure is reported through the common `zlink::framework` error model. The form you receive it
in is one of two, depending on the consumption method.

- `result_t` — for `.result()` or a callback submit. Branch with `operator bool`, access detail with
  `error()`.
- A `framework_exception_t` exception — for `co_await` and `fetch<T>()`. It has `kind()` and
  `what()`.

The same failure is just expressed in two forms — the classification is identical.

## Error Kind Mapping

| kind | When | Automatic retry |
|------|------|-----------|
| `protocol_error` | Invalid configuration/input: bad base_url/scheme, timeout 0 or below, 0-byte response body cap, empty header name, path not starting with `/`, multiple body sources, an unconfigured coroutine execute scheduler, https on a build with no OpenSSL, response JSON decode failure, corrupted gzip/deflate body | ✗ |
| `unavailable` | Transport failure (connection refused/dropped, TLS verification failure), an uninitialized client (used after default-constructing `client_t{}`) | ✓ |
| `deadline_exceeded` | Client/request timeout exceeded. On a coroutine client, the timeout is computed from the moment it's registered with the scheduler queue | ✓ |
| `rejected` | Response body cap exceeded, decompressed size cap exceeded | ✗ |
| `internal_failure` | 4xx/5xx on the typed path, redirect limit exceeded, an unsupported redirect location, proxy CONNECT rejected | ✗ |

A configuration error (`protocol_error`) is deliberately distinguished from a transport
failure — because it's a code bug, so retrying is meaningless. It's often thrown right away, at
setter/`build()` time.

## The result_t Pattern

```cpp
auto result = client.get ("/players/7281").submit<player_profile_t> ().result ();

if (!result) {
    const auto *error = result.error ();
    switch (error->kind ()) {
        case zlink::framework::framework_error_kind_t::deadline_exceeded:
            metrics.count ("player_lookup.timeout");
            break;
        case zlink::framework::framework_error_kind_t::protocol_error:
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
    using kind_t = zlink::framework::framework_error_kind_t;
    if (error.kind () == kind_t::unavailable || error.kind () == kind_t::deadline_exceeded) {
        schedule_retry ();
    } else {
        report_permanent_failure (error.what ());
    }
}
```

## Which Side Is 4xx/5xx On

- `submit<T>()`/`fetch<T>()` (typed): **a failure** — `internal_failure`,
  "HTTP request failed with status 404".
- `submit_raw()`: **a success** — you branch on the status directly
  ([6. Handling Responses](06-handling-responses.en.md)).

If the business logic cares about a status like 404/409, use the raw path; if "200 + DTO, otherwise
a failure" is right, use the typed path.

## The Kinds Automatic Retry Covers

`retry(attempts)` ([Chapter 10](10-redirects-retries-cookies.en.md)) automatically retries
`unavailable` and `deadline_exceeded`. Using the same criterion when writing your
own retry loop keeps it consistent. Neither `framework_exception_t` nor `result_t` carries a flag
telling you whether a failure is retriable, so decide on the kind.

```cpp
using kind_t = zlink::framework::framework_error_kind_t;

for (int attempt = 0;; ++attempt) {
    auto result = client.get ("/ready").submit_raw ().result ();
    const bool retriable = !result
                           && (result.error_kind () == kind_t::unavailable
                               || result.error_kind () == kind_t::deadline_exceeded);
    if (result || attempt >= 3 || !retriable) {
        return result;
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (200 << attempt));
}
```

[← Table Of Contents](README.en.md)
