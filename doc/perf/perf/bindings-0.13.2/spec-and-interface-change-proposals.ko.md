# spec·public interface 변경 의견 (core 0.13.2 bindings 성능 작업)

> 이 문서는 **적용하지 않은 제안만** 모은다. 작업 중에는 반영하지 않으며,
> 작업 완료 후 사용자에게 전달해 판단을 받는다.
>
> 규칙: 작업 중 public interface 변경 금지, core spec 변경 금지, 계약 완화 금지.
> 목표 미달이 남으면 `보류`로 확정하고 다음 항목으로 이동한다.

## 1. core spec 변경 의견

| # | 대상 파일 | 현재 서술의 문제 | 제안 | 근거 | 발견 시점 |
|---:|---|---|---|---|---|
| 1 | `core/doc/spec/core/socket/01-pair.ko.md:101`, `06-dealer.ko.md:120` (및 en) | "모든 실패가 part를 소비한다"고 규정한다. 확정된 계약은 그 반대다: **send/recv가 실패하면 소유권은 애플리케이션이 갖는다. 반만 보내거나 반만 받는 상태는 존재하지 않는다.** transaction admission 이전 거부든 이후 전송 실패든 구분 없이 동일하다. | "실패한 send는 어떤 part도 소비하지 않고 전부 호출자 소유로 남긴다. 부분 전송·부분 수신 상태는 존재하지 않는다"로 다시 쓴다. admission 전후 예외를 두지 않는다. | 사용자 확정 계약 (2026-08-27). all-or-nothing record 원자성과 일관된다. 구현이 admission 이후 실패에서 part를 소비한다면 그것은 core 결함이며 회귀 test와 함께 수정 대상이다. | 2026-08-27 |

## 2. bindings spec 변경 의견

| # | 대상 파일 | 현재 서술의 문제 | 제안 | 근거 | 발견 시점 |
|---:|---|---|---|---|---|
| 1 | `bindings/doc/spec/cpp/README.ko.md:470` (및 en). **다른 언어 binding spec에도 같은 패턴이 있는지 확인 필요** | "같은 native handle의 모든 C++ outbound 경로는 binding-owned record-attempt gate를 공유한다"고 규정해 **모든 송신을 소켓 단위 mutex로 직렬화**하도록 강제한다. 이 gate의 두 목적이 모두 core 계약과 중복된다. ① multipart 시퀀스 인터리빙 방지: core spec `02-message.ko.md:407` "다른 sender의 message part가 이 sequence 사이에 삽입되지 않도록 socket별 transaction state가 send 경로를 보호한다". ② close vs in-flight submit 경합: core spec `socket/README.ko.md:50` "close는 fail-fast lifecycle gate를 사용한다. 다른 thread가 같은 핸들에서 admitted API나 callback을 실행 중이면 EBUSY, close가 accepted된 뒤 새 API 진입은 ESHUTDOWN". 게다가 core spec `socket/README.ko.md:46`은 "send는 여러 thread에서 동시 호출을 허용하는 hot path"로 규정하는데, binding gate가 그 동시성을 소켓 단위로 전부 없앤다. | gate 공유 요구를 삭제한다. close 경합은 core의 lifecycle gate(EBUSY/ESHUTDOWN)를 사용하도록 binding close 경로를 맞추고, multipart 순서는 core transaction state에 위임한다. 최소한 단일 part 송신은 gate 대상에서 제외한다. | core spec `02-message.ko.md:407`, `socket/README.ko.md:46`, `socket/README.ko.md:50`. C++ Single tcp PAIR small-message 경합 비용 제거 기대. 4개 언어 binding 공통 영향. | 2026-08-27, C++ cross-cutting run |

## 3. public interface 변경 의견

| # | 대상 | 제안 | 이 변경이 필요한 이유 | 변경하지 않아 남은 비용 | 발견 시점 |
|---:|---|---|---|---|---|
| | | | | | |

## 4. 이미 반영된 항목 (참고)

| # | 대상 | 반영 내용 | 승인 |
|---:|---|---|---|
| 1 | `core/doc/spec/core/socket/07-router.{ko,en}.md`, `bindings/doc/spec/README.{ko,en}.md` | route 없는 terminal reply의 disposition을 `ZLINK_SUBMIT_NOT_CONNECTED`/`ENOTCONN`으로 명문화. 대상 completion pipe 미발견과 커밋 도중 대상 소멸 두 경우 모두 포함하고, backpressure가 아니므로 `ZLINK_POLLOUT`으로 재시도가 가능해지지 않음을 명시. | 사용자 승인 (2026-08-27) |
