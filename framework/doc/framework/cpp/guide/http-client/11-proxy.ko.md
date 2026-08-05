[← 목차](README.ko.md)

# 11. Proxy

사내망에서 외부 API를 호출하거나 egress가 proxy로 강제되는 환경에서 쓴다.

## 기본 사용

proxy 주소는 `http://host:port` 형식이다 (proxy 자체는 http로만 접속).

```cpp
auto client = zlink::http_client::client_t::create ("https://api.partner-game.example")
                .proxy ("http://egress-proxy.example.internal:3128")
                .build ();

// 이후 모든 요청이 proxy를 경유한다 — 호출 코드는 동일
auto seasons = client.get ("/v1/seasons").fetch<season_list_t> ();
```

## 동작 방식

target scheme에 따라 표준 proxy 프로토콜을 따른다. 호출자가 구분할 필요는 없다.

| target | 방식 |
|--------|------|
| `http://` | absolute-form 전달 — request line에 전체 URL을 실어 proxy가 중계 |
| `https://` | `CONNECT host:port`로 tunnel을 연 뒤, tunnel 안에서 TLS handshake |

`https://` target은 proxy를 지나도 **end-to-end TLS**다. proxy는 암호화된
바이트만 중계하며 내용을 볼 수 없고 server certificate/hostname 검증도 origin
기준으로 그대로 수행된다.

## Proxy 인증

인증이 필요한 proxy(`407 Proxy Authentication Required`)는
`proxy_basic_auth`로 자격을 준다. absolute-form 요청과 `CONNECT` 양쪽에
`Proxy-Authorization: Basic ...`이 실린다.

```cpp
auto client = zlink::http_client::client_t::create ("https://api.partner-game.example")
                .proxy ("http://egress-proxy.example.internal:3128")
                .proxy_basic_auth ("svc-matchmaker", proxy_password)
                .build ();
```

인증 없이 407을 받으면 그 응답이 그대로 반환된다(자동 재시도 없음). `CONNECT`가
거부되면 `request_failed`("proxy CONNECT failed with status 407")로 닫힌다.

## connection pool과의 관계

pool 키에 proxy가 포함되므로, proxy 경유 연결(CONNECT tunnel 포함)도 같은
origin이면 재사용된다. proxy 설정을 바꾸려면 새 client를 만든다 — client 생성
후에는 변경할 수 없다.

[다음: 압축 →](12-compression.ko.md)
