[← 목차](README.ko.md)

# 1. 개요

## 무엇인가

`@zlink-systems/http-client`는 Node 애플리케이션이 HTTP API를 호출할 때 쓰는 client-side
산출물이다. Node에는 global `fetch`가 있지만 gzip을 자동 해제하고 cookie jar·redirect
횟수 제한·세밀한 proxy/TLS 제어가 부족해 zlink 계약과 어긋난다. 이 client는 undici
저수준 위에 fluent builder를 얹어 그 복잡성을 숨기고 framework의 에러·코덱 모델과 맞춘다.

```ts
const profile = await client.get('/players/7281').submit<PlayerProfile>();
```

JSON 전용 client가 아니다. 일반 HTTP client이며 typed JSON 경로
(`body(dto)` / `submit<T>()`)는 그 위에 얹은 편의 계층이다.

## 설계 원칙

- **fluent builder.** client 구성과 request 구성 모두 메서드 체인으로 쓴다.
- **공개 표면에 undici 없음.** `Dispatcher`, `Agent`, `request` 타입은 공개 API에
  드러나지 않는다. undici 의존은 runtime 구현 안에 갇힌다.
- **네이티브 래핑.** 전송은 undici에 위임하되, 계약과 의미론이 다른 부분(cookie jar,
  redirect 루프, retry, 압축 통제)은 얇은 래퍼에서 직접 구현한다.

## 백엔드 선택 — `fetch`가 아닌 undici

parity 구현은 undici 저수준 `request`를 쓴다. `fetch`는 `follow_redirects(n)`에 대응하는
숫자 redirect 한도가 없고 gzip을 자동 해제해 streaming·헤더 제거·decoded limit 의미론을
깨며, client별 proxy/TLS 제어가 제한적이다. undici `request`는 auto-redirect·
auto-decompress·cookie를 하지 않아 래퍼가 의미론을 통제할 수 있다.

## 실행 모델

- `submitRaw()` / `submit<T>()` / `download(sink)`는 `Promise`를 돌려준다. `await`하는
  동안 HTTP I/O는 libuv event loop의 비동기 소켓에서 처리되고 **event loop 스레드는
  점유되지 않는다.**
- Node에는 동기 blocking HTTP 접근이 없다(blocking 경로 없음).

## 기능 한눈에 보기

- 메서드: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- body: typed JSON · raw · form-urlencoded · multipart/form-data · chunked streaming 업로드
- 응답: raw · typed JSON · streaming 다운로드
- connection pool(undici), redirect 추적, transport retry, cookie jar
- 인증: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS 검증, test certificate trust
- HTTP proxy
- gzip/deflate 응답 투명 해제

[다음: 시작하기 →](02-getting-started.ko.md)
