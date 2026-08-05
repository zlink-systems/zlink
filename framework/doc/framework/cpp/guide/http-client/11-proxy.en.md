[← Table Of Contents](README.en.md)

# 11. Proxy

Used to call an external API from an internal network, or in an environment where egress is forced
through a proxy.

## Basic Usage

The proxy address takes the form `http://host:port` (the proxy itself is only reached over http).

```cpp
auto client = zlink::http_client::client_t::create ("https://api.partner-game.example")
                .proxy ("http://egress-proxy.example.internal:3128")
                .build ();

// From here, every request goes through the proxy — the calling code is unchanged
auto seasons = client.get ("/v1/seasons").fetch<season_list_t> ();
```

## How It Works

Standard proxy protocol is followed based on the target scheme. The caller doesn't need to
distinguish between them.

| Target | Method |
|--------|------|
| `http://` | absolute-form delivery — carries the full URL in the request line for the proxy to relay |
| `https://` | opens a tunnel with `CONNECT host:port`, then performs the TLS handshake inside the tunnel |

Even through a proxy, an `https://` target is **end-to-end TLS**. The proxy relays only encrypted
bytes and can't see the content, and server certificate/hostname verification still runs against the
origin as usual.

## Proxy Authentication

For a proxy that requires authentication (`407 Proxy Authentication Required`), give credentials
with `proxy_basic_auth`. `Proxy-Authorization: Basic ...` is carried on both an absolute-form
request and `CONNECT`.

```cpp
auto client = zlink::http_client::client_t::create ("https://api.partner-game.example")
                .proxy ("http://egress-proxy.example.internal:3128")
                .proxy_basic_auth ("svc-matchmaker", proxy_password)
                .build ();
```

If a 407 is received with no authentication configured, that response is returned as-is (no
automatic retry). If `CONNECT` is rejected, it closes with `request_failed`
("proxy CONNECT failed with status 407").

## Relationship With The Connection Pool

Since the pool key includes the proxy, a proxy-routed connection (including the CONNECT tunnel) is
also reused for the same origin. To change the proxy setting, create a new client — it can't be
changed after the client is created.

[Next: Compression →](12-compression.en.md)
