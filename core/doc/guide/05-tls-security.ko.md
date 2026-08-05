---
title: "TLS와 WSS"
---

[English](05-tls-security.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Transport 가이드](04-transports.ko.md) | [다음: Raw socket monitoring](06-monitoring.ko.md)
<!-- zlink-nav:end -->

# TLS와 WSS

> **이 장이 답하는 것** — OpenSSL 기반 `tls://`·`wss://` endpoint를 구성하는 방법을
> 설명한다.

Core는 OpenSSL을 사용해 암호화된 `tls://`와 `wss://` endpoint를 제공한다. Bind나 connect 전에
raw socket에 TLS를 설정한다.

## Server

```c
void *server = zlink_socket(ctx, ZLINK_CORE_SOCKET_ROUTER);
zlink_set_tls_server(server, "server.crt", "server.key", 0);
/* Bind가 handshake 경로를 시작하기 전에 certificate와 key를 설정한다. */
zlink_bind(server, "tls://*:5555");
```

마지막 인자를 0이 아닌 값으로 설정하면 client certificate를 요구한다. Certificate와 private-key
file은 PEM 형식을 사용한다.

## Client

```c
void *client = zlink_socket(ctx, ZLINK_CORE_SOCKET_DEALER);
zlink_set_tls_client(client, "ca.crt", "server.example.com", 0);
/* Connection handshake에서 hostname을 검증한다. */
zlink_connect(client, "tls://server.example.com:5555");
```

CA path, hostname과 system-trust flag가 peer 검증 방식을 결정한다. Test certificate를 연결하기
위해 production에서 certificate나 hostname 검증을 비활성화하지 않는다.

## WSS

`wss://`는 같은 TLS 설정을 WebSocket transport에 적용한다. STREAM server는 외부
WebSocket/TLS client를 받고 ZMP socket type은 zlink WSS endpoint에 connect할 수 있다.

Handshake failure는 raw [socket monitor](06-monitoring.ko.md)로 확인한다.
