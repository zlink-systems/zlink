# C++ Stream Connector 가이드

C++ STREAM client connector 제품군의 공식 사용자 가이드다.

## 목차

| 문서 | 내용 |
|------|------|
| [01 — 개요](01-overview.ko.md) | 제품군 구성, 배포 단위, 지원 엔진 |
| [02 — 시작하기](02-getting-started.ko.md) | 설치, CMake 연결, 첫 연결과 request |
| [03 — Connector 옵션](03-connector-options.ko.md) | endpoint, timeout, heartbeat, reconnect, dispatch mode |
| [04 — 패킷 송신](04-sending.ko.md) | send, request, metadata, 압축, codec |
| [05 — 패킷 수신](05-receiving.ko.md) | on(), dispatch(), wait_for(), 이벤트 callback |
| [06 — 연결 생명주기](06-lifecycle.ko.md) | connect, close, 상태 전이, reconnect, heartbeat |
| [07 — 오류 처리](07-error-handling.ko.md) | result_t, error_code_t, 오류별 대응 |
| [08 — E2E 클라이언트](08-e2e-client.ko.md) | async(), co_await, task_t, perf scenario |
| [09 — 엔진 어댑터](09-engine-adapters.ko.md) | Unreal, Godot, Axmol wrapper 사용법 |
| [10 — 패키징](10-packaging.ko.md) | vcpkg, Conan, CMake find_package, build features |
| [11 — 성능 테스트](11-performance.ko.md) | smoke, scale 실행, report 해석 |

## 빠른 참조

**일반 C++ client**: [시작하기](02-getting-started.ko.md) → [패킷 송신](04-sending.ko.md) → [패킷 수신](05-receiving.ko.md)

**서버 e2e/perf scenario**: [E2E 클라이언트](08-e2e-client.ko.md) → [성능 테스트](11-performance.ko.md)

**엔진 통합**: [엔진 어댑터](09-engine-adapters.ko.md) → [연결 생명주기](06-lifecycle.ko.md)

**배포/빌드**: [패키징](10-packaging.ko.md)
