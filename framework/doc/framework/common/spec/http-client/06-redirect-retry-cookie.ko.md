# 6. Redirect · Retry · Cookie

> [공통 계약 목차](README.ko.md)

세 기능 모두 네이티브 자동 기능을 끄고 래퍼가 구현한다. 5개 언어의 동작이
바이트 수준까지 같아야 하는 핵심 계약 영역이다.

## 6.1 Redirect

`followRedirects(max)` 활성 시(무인자 기본 5):

| status | 메서드 rewrite | body |
| --- | --- | --- |
| 301, 302 (GET/HEAD) | 보존 | 보존 |
| 301, 302 (POST) | **GET으로 변경** | **제거** |
| 303 | **항상 GET** | **제거** |
| 307, 308 | 보존 | 보존 (streaming body는 rewind 불가라 드롭) |

- `Location`은 절대/상대 URL을 지원한다. 해석 불가능한 형식은 `ProtocolError`이며 원 요청을
  다시 전송하지 않는다.
- **cross-origin으로 이동하면 `Authorization` 헤더를 제거**한다.
  same-origin(scheme+host+port 동일)이면 보존한다.
- 한도 초과는 `ProtocolError`이며 원 요청을 다시 전송하지 않는다.
- redirect 중간 응답의 body는 소비(drain)하되 사용자에게 노출하지 않는다
  ([4장 §4.4](04-response-model.ko.md)).

## 6.2 Retry와 timeout

- `retry(attempts)`: 총 시도 = 1 + attempts.
- **자동 retry 대상은 전송 계층 실패와 timeout뿐이다.** HTTP status(4xx/5xx)는
  재시도하지 않는다.
- streaming(업로드 provider 또는 download sink)이 있으면 재시도하지 않는다.
- 시도 간 지연은 **지수 백오프 + full jitter**다: 상한 =
  `min(1초, 50ms × 2^attempt)`, 실제 지연 = `[0, 상한]` 균등 무작위
  (2026-07-12 R3 승격 — 고정 50ms에서 개정. 고정 지연은 장애 서버에 대한
  동시 재돌진을 유발한다).
- timeout은 **시도(attempt)당** 적용한다. Timeout 실패는 설정한 횟수 안에서 자동 retry 대상이다.
  따라서 `retry(n)` + timeout 조합이 "응답이 timeout 안에 안 오면 n회
  재시도"의 표준 표현이다.
- 재시도 전체를 아우르는 총 데드라인은 계약에 없다(최악 대기 ≈
  시도 수 × timeout + 지연). 언어 편차: cpp 코루틴 경로만 재시도에 걸친
  총 데드라인을 추가로 강제한다. cpp 동기 경로가 총 데드라인 없이
  blocking하는 것은 구현 결함으로 plan 문서에서 추적하며, 총 데드라인의
  계약화는 [R3](10-revision-candidates.ko.md)과 함께 검토한다.

## 6.3 Cookie jar

`cookies()` 활성 시, 의도적으로 좁힌 RFC 6265 부분집합:

- 저장 키는 **host 정확 일치**(`Domain` 속성 무시).
- 지원 속성: `Path`(기본 `/`, path-segment prefix 매칭), `Secure`(https에만
  전송), `Max-Age`(`<= 0`이면 즉시 삭제). `Expires`/`HttpOnly`/`SameSite`는
  무시한다.
- host당 최대 **128개**. 초과 시 가장 오래된 것부터 제거.
- malformed `Set-Cookie`는 조용히 무시한다.

## 6.4 Connection 재사용

- keep-alive/pool은 전송 스택에 위임한다(dotnet/java/node). cpp는 자체 pool을
  가지며(키: `scheme|host:port[|proxy]`) 재사용 연결의 교환 실패는 idempotent
  메서드(GET/HEAD/OPTIONS)에 한해 새 연결로 1회 재수행한다.
- streaming 업로드는 pool을 경유하지 않고 새 연결을 쓴다(cpp 명시 규칙,
  타 언어는 스택 내부 처리).
