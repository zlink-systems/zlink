[← 목차](README.ko.md)

# 10. Redirect · Retry · Cookie

이 세 기능은 .NET 네이티브 핸들러가 제공하는 의미론이 ZLink 계약과 달라서, 네이티브
동작을 끄고 **래퍼에서 직접 구현**한다.

## Redirect

`FollowRedirects(max)`로 활성화한다. 네이티브 auto-redirect는 끄고 래퍼가 redirect
루프를 돈다.

- 추적 상태: `301`, `302`, `303`, `307`, `308` + `Location` 헤더.
- 메서드 rewrite: `303`, 또는 `301`/`302` + `POST` → `GET`으로 바꾸고 본문을 제거한다.
- **`Authorization` 보존 규칙**: same-origin(scheme+host+port 동일) redirect에서는
  `Authorization`을 보존하고 cross-origin으로는 제거한다. .NET auto-redirect는
  same-origin에서도 `Authorization`을 보존하지 않으므로 래퍼 루프가 필요하다.
- `max` 횟수를 넘기면 `InternalFailure`로 실패한다.
- 지원 location: 절대(`http(s)://...`)와 path-absolute(`/...`). 그 외 상대 경로는
  지원하지 않는다.

## Retry

`Retry(attempts)`로 transport 실패를 재시도한다(지수 백오프 + full jitter 간격(기본 50ms, 시도마다 2배, 상한 1초, 0~상한 무작위)).

- 재시도 대상: **retriable transport 실패**(연결 오류, timeout 등). status 코드(4xx/5xx)
  자체는 재시도하지 않는다.
- **streaming(다운로드 sink 또는 업로드 provider)은 rewind 불가이므로 retry에서
  제외**된다.

## Cookie jar

`Cookies()`로 활성화한다. 네이티브 `CookieContainer`(RFC 6265 전체) 대신 래퍼 소유
jar를 쓴다. 좁은 의미론을 따른다:

- host 정확 매칭으로 저장(`Domain` 속성 미지원).
- 기본 `Path=/`. `Path`/`Secure`/`Max-Age` 속성만 해석하고 `Domain`/`Expires`는 무시.
- `Max-Age<=0`이면 삭제.
- secure cookie는 secure(https) 요청에만 전송.
- host당 최대 128개, 초과 시 가장 오래된 것부터 제거.

[다음: Proxy →](11-proxy.ko.md)
