[← 목차](README.ko.md)

# 10. Redirect · Retry · Cookie

이 세 기능은 `java.net.http`가 제공하는 의미론이 ZLink 계약과 달라서 **래퍼에서 직접
구현**한다.

## Redirect

`followRedirects(max)`로 활성화한다. `HttpClient.Redirect.NEVER`로 두고 래퍼가 redirect
루프를 돈다(`Redirect` enum에는 횟수 개념이 없다).

- 추적 상태: `301`, `302`, `303`, `307`, `308` + `Location` 헤더.
- 메서드 rewrite: `303`, 또는 `301`/`302` + `POST` → `GET`으로 바꾸고 본문을 제거한다.
- **`Authorization` 보존 규칙**: same-origin(scheme+host+port 동일) redirect에서는
  `Authorization`을 보존하고 cross-origin으로는 제거한다.
- `max` 횟수를 넘기면 예외로 실패한다.
- 지원 location: 절대(`http(s)://...`)와 path-absolute(`/...`).
- redirect 루프는 `CompletionStage` 체인으로 합성되어 hop 사이에 스레드를 점유하지 않는다.

## Retry

`retry(attempts)`로 transport 실패를 재시도한다(지수 백오프 + full jitter 간격(기본 50ms, 시도마다 2배, 상한 1초, 0~상한 무작위), async 합성).

- 재시도 대상: **retriable transport 실패**(`IOException` — 연결 오류, timeout 등).
  status 코드(4xx/5xx) 자체는 재시도하지 않는다.
- **streaming(다운로드 sink 또는 업로드 provider)은 rewind 불가이므로 retry에서
  제외**된다.

## Cookie jar

`cookies()`로 활성화한다. JDK `CookieManager`(RFC 6265 전체) 대신 래퍼 소유 jar를 쓴다.
좁은 의미론을 따른다:

- host 정확 매칭으로 저장(`Domain` 속성 미지원).
- 기본 `Path=/`. `Path`/`Secure`/`Max-Age` 속성만 해석하고 `Domain`/`Expires`는 무시.
- `Max-Age<=0`이면 삭제.
- secure cookie는 secure(https) 요청에만 전송.
- host당 최대 128개, 초과 시 가장 오래된 것부터 제거.

[다음: Proxy →](11-proxy.ko.md)
