[한국어](05-tls-security.ko.md)

# TLS and WSS

Core supports encrypted `tls://` and `wss://` endpoints through OpenSSL.
Configure TLS on the raw socket before bind or connect.

## Server

```c
void *server = zlink_socket(ctx, ZLINK_CORE_SOCKET_ROUTER);
zlink_set_tls_server(server, "server.crt", "server.key", 0);
/* The certificate and key must be ready before bind starts the handshake path. */
zlink_bind(server, "tls://*:5555");
```

Set the last argument to a non-zero value to require a client certificate.
Certificate and private-key files use PEM format.

## Client

```c
void *client = zlink_socket(ctx, ZLINK_CORE_SOCKET_DEALER);
zlink_set_tls_client(client, "ca.crt", "server.example.com", 0);
/* The hostname is checked during the connection handshake. */
zlink_connect(client, "tls://server.example.com:5555");
```

The CA path, hostname, and system-trust flag determine peer verification.
Do not disable certificate or hostname verification in production merely to
make a test certificate connect.

## WSS

`wss://` applies the same TLS configuration to WebSocket transport. A STREAM
server accepts external WebSocket/TLS clients; ZMP socket types can connect to
a zlink WSS endpoint.

Monitor handshake failures through the raw [socket monitor](06-monitoring.en.md).
