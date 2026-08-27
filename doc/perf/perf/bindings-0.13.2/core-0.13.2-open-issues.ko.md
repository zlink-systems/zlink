# Core 0.13.2 bindings 성능 작업 이슈 해결 결과

> 해결일: 2026-08-27 · branch: `core-0.13.2-bindings-performance` · 상태: 기존
> 미해결 이슈 7건 모두 해결·검증 완료 · 계약 기준: `core/doc/spec/core/`

## 1. 적용한 Core 계약

기존 문서의 “실패한 모든 send는 part를 소비하지 않는다”는 전제는 Core 스펙과
달랐다. 구현과 binding은 다음 계약을 기준으로 정리했다.

| API | 성공 | 실패 |
|---|---|---|
| sync native part API (`send_part`, `publish_part`, request/reply part) | 유효한 현재 part를 소비한다 | 일반 실패도 유효한 현재 part를 소비하고 열린 helper sequence를 abort한다 |
| STREAM routed sync send | part를 소비한다 | `BACKPRESSURED`/`EAGAIN`만 재시도를 위해 part를 보존한다 |
| `zlink_send_async` | `OK`이면 Core로 소유권이 이전된다 | non-`OK`이면 operation id는 0이고 호출자 소유권을 보존한다 |
| receive | 성공하면 반환 part의 소유권이 호출자로 이전된다 | 실패하면 전달된 part가 없다 |

multipart의 all-or-nothing은 **peer에 부분 record를 노출하지 않는 전송 단위의
원자성**이다. 이미 sync Core API에 전달한 part의 소유권을 실패 뒤 호출자에게
되돌린다는 뜻이 아니다. 고수준 binding이 non-OK 뒤 public `Message`를 보존해야
하면 native clone/staging을 사용하며, 이 동작은 Core native part 계약과 구분한다.

close는 기존 계약대로 fail-fast다. 같은 handle에 admitted API나 callback이 있으면
`ZLINK_CLOSE_BUSY`/`EBUSY`, close가 accepted된 뒤의 새 진입은
`ZLINK_CLOSE_SHUTDOWN`/`ESHUTDOWN`이다. binding이 BUSY를 숨기거나 자동 재시도하지
않는다.

## 2. 기존 7개 이슈의 해결 결과

### 2.1 socket handle heap-use-after-free

해결했다.

- public opaque pointer 뒤에 Context가 소유하는 `socket_public_handle_t`를 두었다.
- 모든 public socket API는 socket/tag를 읽기 전에 stable handle의 atomic pin을
  획득한다.
- close admission은 같은 handle의 다른 pin과 원자적으로 경합한다. 다른 API가
  진행 중이면 BUSY를 반환하며, accepted close 뒤의 새 pin은 SHUTDOWN으로 거부된다.
- reaper가 ordinary mailbox lifetime을 끝내도 마지막 public pin이 빠질 때까지 실제
  socket 파괴를 미룬다. Context 종료 시 stable handle storage 자체는 정리된다.
- hot path에는 전역 lock이나 호출별 heap allocation을 추가하지 않았다. API 진입의
  CAS 기반 pin 획득과 종료의 atomic release가 추가 비용의 전부다.

`test_public_inproc_multipart_send`, close/send 경합, monitor/poller, request/reply와
sanitizer 회귀 검증에서 UAF가 재발하지 않았다.

### 2.2 multipart 실패·소유권·rollback

잘못된 전제를 위 §1의 계약으로 바로잡고 구현을 정리했다.

- sync helper는 성공과 일반 실패에서 현재 native part를 소비하고 sequence를 정확히
  commit/abort한다.
- STREAM `BACKPRESSURED`만 part를 보존한다. Go와 Rust binding도 이 예외를 동일하게
  처리한다.
- async submit은 inline attempt부터 pending queue publish까지 하나의 transaction으로
  처리한다. queue/index allocation, operation-id exhaustion, later-part HWM 실패 시
  operation id 0과 원본 part 보존을 보장한다.
- request/reply와 publish 경로의 allocation/exception 실패를 정리하고 completion
  queue를 allocation-free intrusive queue로 변경했다.
- binding은 필요한 곳에서 native clone/staging을 사용해 public object 보존 계약을
  지키되 Core의 sync 소비 계약을 바꾸지 않는다.
- generic `zlink_send_part`는 PAIR/DEALER에서만 허용한다. ROUTER/STREAM은
  `NOT_SUPPORTED`로 거부하고 열린 sequence를 오염시키지 않는다.

### 2.3 Java local Core async hang

해결했다. Core가 inline admission을 완료해 `op_id == 0`을 반환하면 Java
`SendCompletionRegistry`가 pending callback을 기다리지 않고 future를 즉시 완료한다.
Python과 Rust도 같은 계약을 적용한다.

검증 중 발견한 별도 `ReceiveFlowStateContractTest` hang은 binding 문제가 아니었다.
setter가 먼저 admission을 얻으면 concurrent close의 BUSY가 정상인데, 테스트가 열린
DEALER를 남긴 채 `ctx_term`에 들어가 원래 결과를 가렸다. 테스트가 setter join 뒤
cleanup close를 수행하도록 수정했다.

- 원래 경합 계측: BUSY 38/3,000, unexpected 0
- 수정 뒤: 50회 반복, 총 1,000 race iteration, hang 0

### 2.4 안정된 ROUTER route의 산발적 `ENOENT`

수명·route-ready 경합과 result mapping을 정리했다.

- Java는 `ENOENT`를 `NOT_FOUND`로 변환하며 `INTERNAL_ERROR`로 오분류하지 않는다.
- paired inproc의 peer ROUTER RID ready 발행을 attach/adopt 소유 경로로 옮겨 RID blob
  read/write 경합을 제거했다.
- routed send 내부 반환값이 public `ENOENT`로 잘못 새는 경로를 정리했다.
- Go routed HWM 회귀 테스트 50회와 Core routed stress 20회, 총 160만 attempt에서
  ENOENT·unexpected failure·bad record가 모두 0이었다.

### 2.5 binding spec의 binding-owned gate 잔재

C++·Go 언어별 한/영 spec을 공통 binding 계약과 맞췄다. binding은 outbound
record-attempt lock/gate, 대기와 재시도 정책을 소유하지 않고 Core 결과를 그대로
노출한다. 고수준 public object 보존을 위한 staging은 직렬화 gate와 별개다.

### 2.6 TSAN 실행 불가와 실제 race

`setarch x86_64 -R`로 ASLR을 비활성화해 이 환경의
`ThreadSanitizer: unexpected memory mapping`을 우회하고 TSAN을 실제 실행했다.
그 결과 다음 race를 수정했다.

1. paired inproc ready 처리 중 peer routing-id blob read/write race
2. reaper와 I/O thread 사이 `destroy_pending` race
3. auto-HWM periodic task id의 teardown/recreate race

추가로 테스트 callback이 condition-variable predicate를 게시한 뒤 lock 밖에서
`notify_all()`을 호출해 stack probe 파괴와 겹치던 test-only race를 수정했다. notify를
같은 mutex 안에서 완료하고 임시 `sleep(10ms)`를 제거했다.

최종 TSAN은 async multipart 14/14와 ROUTER invalid-reply 1/1, 합계 15/15에서 runtime
warning 0이었다.

### 2.7 Go header hash allowlist

Core package의 공개 header를 8개 binding native workspace에 동기화하고 Go raw header
allowlist와 hot-path inventory를 갱신했다. 최종 header hash는
`319fe117...` 계열로 일치하며 `RawCore11Allowlist`, `HotPathCostInventory`, `go vet`와
전체 Go 테스트가 통과했다.

## 3. 최종 계약 감사에서 추가로 해결한 항목

- ROUTER reply의 `NULL` 또는 closed FINAL part가 빈 성공 reply로 전송되던 문제를
  prevalidation과 move 결과 검사로 막았다. 잘못된 sequence는 abort하며 같은
  RID/sequence의 다음 정상 reply만 전달된다.
- peer-RID-only 단일-part async fast path가 admission gate를 우회해 같은 target의 뒤
  요청이 추월할 수 있던 문제를 제거했다. 모든 async submit이 같은 transactional
  inline/pending 경로를 사용한다.
- request timeout close 과정에서 pending unbound inproc pipe가 materialize되지 않아
  reaper가 멈추던 문제와 request-completion mailbox wakeup 손실을 수정했다.
- monitor handler 등록 직후 callback self-close가 등록 API의 내부 public pin 때문에
  BUSY가 되던 문제를 수정했다. registry pin으로 state/socket 수명을 보호한 뒤 immediate
  dispatch arm 전에 public pin을 해제한다. focused stress 50/50과 monitor contract
  19/19가 통과했다.
- native close-vs-flow-setter 테스트도 initial BUSY를 캡처하고 join 뒤 close 재시도 성공을
  확인한 후에만 socket을 closed로 표시하도록 보강했다.
- .NET package entry 검증은 `unzip -Z1` 호환성을 가정하지 않고 Python `zipfile`을
  사용한다. 이 환경의 비호환 `unzip` wrapper가 목록 조회 대신 현재 디렉터리에 package를
  풀던 문제를 제거했다.

## 4. 최종 검증

모든 binding은
`.artifacts/wsl/install/zlink-core/0.13.2`의 동일한 local Core package를 사용했다.

| 범위 | 결과 |
|---|---|
| Core CTest | 103/103 통과, 176.11초 |
| Core flow-state 보강 | binary 22/22, CTest 1/1 통과 |
| ASan / LeakSanitizer | 최신 대상 44/44, 진단 0 |
| UBSan | 최신 대상 44/44, 진단 0 |
| TSAN | 최신 대상 15/15, runtime warning 0 |
| C | contract 7/7, sample 6/6 |
| C++ | contract 14/14, sample 7/7 |
| C++ send/close stress | 47,580 attempts, ownership failure 0, bad record 0, unexpected 0 |
| .NET | 180/180, sample 7/7 |
| Go | `go test ./...`, `go vet ./...`, raw/hot-path guards, sample 7/7 |
| Java | unit, integration, Netty, Kotlin sample 7/7, Java sample 7/7 |
| Node | 68/68, Node/JavaScript sample 14/14 |
| Python | 112/112, sample 7/7 |
| Rust | 14개 suite와 sample 통과; workspace all-target package test 통과 |
| local package | Core와 8개 first-party binding package 생성 완료 |

Java 검증은 class-file target과 실행 VM을 맞추기 위해 JDK 22를 사용했다. Rust는
edition 2024를 지원하는 Cargo/Rust 1.97.1을 사용했고, Python build/test 도구는
`.artifacts/wsl` 아래 격리 venv에서 실행했다.

## 5. 결론

기존 미해결 이슈 7건과 최종 계약 감사에서 재현한 추가 결함을 모두 해결했다. Core
public API나 반환 계약은 확장·완화하지 않았으며 `core/doc/spec/core/` 문서는 수정하지
않았다. 생성된 local native/package artifact는 검증 입력이며 commit 대상이 아니다.
