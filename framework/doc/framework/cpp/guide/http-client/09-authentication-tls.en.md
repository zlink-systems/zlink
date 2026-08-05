[← Table Of Contents](README.en.md)

# 9. Authentication And TLS

## HTTP Basic

`basic_auth(user, password)` carries `Authorization: Basic <base64>` on every request.

```cpp
auto registry = zlink::http_client::client_t::create ("https://registry.example.internal")
                  .basic_auth ("matchmaker-svc", service_password)
                  .build ();
```

## Bearer Token

`bearer_token(token)` carries `Authorization: Bearer <token>`. Used for OAuth/JWT-based APIs.

```cpp
auto api = zlink::http_client::client_t::create ("https://game-api.example.internal")
             .bearer_token (access_token)
             .build ();
```

For a service whose token gets refreshed on expiry, either re-create the client per token lifetime,
or overwrite it per request with `header("authorization", ...)`.

```cpp
client.get ("/players/7281")
  .header ("authorization", "Bearer " + token_provider.current ())
  .submit<player_profile_t> ();
```

(For proxy authentication, see `proxy_basic_auth` in [11. Proxy](11-proxy.en.md).)

If `follow_redirects()` is on and the redirect target is a different origin from the original
request (differing in scheme, host, or port), the `Authorization` header isn't sent again. This is a
default protection so a different host doesn't receive Basic or Bearer credentials through a
redirect response. A redirect within the same origin keeps the existing authentication header.
Other secret headers you added directly via `default_header` or a per-request `header` can't be
distinguished from an ordinary header, so they're not subject to this automatic removal.

## HTTPS Default Behavior

An `https://` endpoint always performs the following. **There is no option to turn it off** — the
contract is that TLS verification is never implicitly disabled.

- TLS handshake
- Server certificate verification (defaults to the system CA path)
- Hostname verification

A verification failure (an untrusted certificate, a hostname mismatch) is reported as a transport
failure.

## Trusting A Private/Test Certificate

For an internal CA or a development certificate, trust is **explicitly** added with
`trust_certificate_file`.

```cpp
auto internal_api =
  zlink::http_client::client_t::create ("https://game-api.staging.internal:8443")
    .trust_certificate_file ("/etc/pki/staging-root-ca.pem")
    .build ();
```

## mTLS (Client Certificate)

If the server requires a client certificate, present it with `client_certificate_file(cert, key)`.
It's PEM format.

```cpp
auto settlement =
  zlink::http_client::client_t::create ("https://settlement.partner-bank.example:9443")
    .trust_certificate_file ("/etc/pki/partner-bank-ca.pem")
    .client_certificate_file ("/etc/pki/matchmaker-client.crt.pem",
                              "/etc/pki/matchmaker-client.key.pem")
    .build ();
```

If no certificate is presented, the server rejects the handshake and it ends as a transport failure.

## Builds Without OpenSSL

In a build without OpenSSL, an `https://` request closes with `request_protocol_error`
("HTTPS support requires OpenSSL"). `http://` is unaffected.

[Next: Redirect · Retry · Cookie →](10-redirects-retries-cookies.en.md)
