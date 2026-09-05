# Core `send_routing_id` abort (`Assertion failed: written`, pipe.cpp:210) — 요약

java framework unit suite가 병합 Core(D-090 `40137f1bd0` + D-B112 `349040d3e6`)에서 test JVM을
SIGABRT로 죽이던 원인은 inproc connect-before-bind 완료 경로(`ctx_inproc_registry.cpp`)의 순서
결함이다. 등록소가 bind 쪽 routing-id preamble을 **bind socket의 admission 뒤에** 썼는데, admission이
pipe를 즉시 종료하는 경우(REJECT 중복 RID, close/disconnect 시 PAIR helper materialization)에는
종료된 pipe에 쓰기가 실패해 `zlink_assert (written)`이 터졌다. 변경은 미커밋 상태다.

- 작업 트리: `/home/hep7/project/zlink-core-b`
- 기준 HEAD: `ccb418b6ee` (detached; explicit-removal NOT_FOUND 수정 포함)
- 패치: `/home/hep7/project/zlink-core-b/core-send-routing-id.patch` (`git add -N <새 test>` 뒤 `git diff HEAD`;
  worktree에 이미 있던 무관한 `diag-node-old-core/`는 제외했다)
- main에는 이 요약만 작성했다. main의 `core/build-dev`, `doc/spec/**`, `bindings/**`, `framework/**`는 수정하지 않았다.

## 유발 java 테스트와 시퀀스

`--info` 실행으로 abort 직전에 실행 중이던 케이스를 확인했다:
`ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`
(`framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeM6ATest.java:613`).
같은 클래스의 `descriptorFenceReplacesEndpointOnlyIntent` 등 replacement 흐름을 쓰는 케이스도 같은 assert에
도달한다(아래 BLOCKERS).

Core 관점의 시퀀스(모두 probe ROUTER, 기본 REJECT 정책, inproc):

1. local(RID L, bind inproc://L)이 peer(RID P, bind inproc://P)에 connect → READY.
2. peer close → local pipe 종료 → `socket_base_api.cpp:1754-1757, 1925`가 inproc 재연결 의도를
   re-arm(`send_reconnect_inproc`) → `socket_base_lifecycle.cpp:1399` `process_reconnect_inproc` →
   `connect_internal` → 아직 bind가 없으므로 registry에 **pending**으로 대기.
3. replacement peer(RID P)가 inproc://P bind → `connect_pending` → pending 연결 admission(READY).
4. java `replacePeerConnection`은 disconnect 없이 다시 `connect` → 직접 경로 → replacement가 L 중복으로
   REJECT하고 D-B112대로 pipe를 즉시 close → local은 그 종료를 보고 다시 re-arm(재연결 루프).
   local 자신도 같은 endpoint 재연결 규칙(`router_admission.cpp:351-357`)으로 이전 pipe를 대체·종료한다.
5. replacement close → endpoint 해제 → 진행 중이던 attempt들의 종료마다 re-arm → **같은 endpoint에 pending
   연결이 2개 이상** 쌓인다.
6. local close → `socket_base_api.cpp:174,188` `materialize_pending_inprocs_before_reap` →
   `ctx.cpp:384`가 PAIR helper 하나를 만들어 pending 전부를 `connect_inproc_sockets(bind_side, materialization)`로
   완료 → `pair.cpp:54`의 PAIR `xattach_pipe`가 두 번째 pipe를 `terminate (false)` →
   `ctx_inproc_registry.cpp:425`(기준 HEAD) `send_routing_id (bind_pipe, ...)` → `write_routing_id_and_flush`가
   `_state != active`로 false → `pipe.cpp:210` abort.

같은 결함의 두 번째 진입: pending 두 개가 같은 RID를 가진 상태에서 REJECT ROUTER가 bind하면
`connect_pending` → `process_command (bind)` → `router_admission.cpp:366,461` REJECT `terminate (true)` →
같은 `:425` 쓰기 → abort. 세 번째 진입: pending 두 개인 endpoint에 `zlink_disconnect`
(`socket_base_endpoint.cpp:1045` materialize) → 같은 경로.

이전 패키지(12:54 Core)가 abort하지 않은 이유: D-B112 이전에는 REJECT가 중복 pipe를 닫지 않고 anonymous로
남겨 두어 connector에 종료·re-arm이 발생하지 않았고, 같은 endpoint에 pending이 2개 쌓이는 5단계가 거의
일어나지 않았다. 결함 자체(`:425`의 순서)는 그 이전부터 있었고 `zlink_connect` 두 번 후 close라는 최소
재현(`test_two_pending_connects_then_close`)으로 드러난다.

## 원인 file:line과 수정

원인 위치(기준 HEAD `ccb418b6ee`):

- `core/src/runtime/core/ctx_inproc_registry.cpp:373` bind socket `process_command (bind)` →
  `:382` connector `send_bind` → `:425` bind 쪽 `send_routing_id`. 직접 연결 경로
  `socket_base_endpoint.cpp:403-406`은 두 preamble을 모두 `send_bind (:436)`·connector `attach_pipe (:467)`
  **앞에** 쓰는데, pending 경로만 admission 뒤에 썼다.
- 종료를 일으키는 admission 소유자는 그대로다: `router_admission.cpp:366,461`(REJECT 즉시 close, D-B112),
  `pair.cpp:54`(PAIR는 pipe 하나만 수용).

수정(단일 소유자 `ctx_inproc_registry_t::connect_inproc_sockets` 안에서 순서만 정렬, 규칙 추가 없음):

1. `ctx_inproc_registry.cpp:346` — bind 쪽 routing-id preamble을 completion lane materialization과
   양쪽 admission보다 앞에 stage한다. 두 preamble이 pipe에 있고 난 뒤에만 어느 socket이든 pipe를 admit하므로
   "종료 뒤 쓰기"가 구조적으로 불가능하다. assert는 그대로 두었다(다운그레이드 아님).
2. `ctx_inproc_registry.cpp:398,410` — bind_side에서 connector의 bind command(`send_bind`)를 bind socket의
   `process_command (bind)`보다 먼저 queue한다. 직접 경로와 같은 순서다. binder의 REJECT `terminate (true)`가
   보내는 pipe_term은 connector mailbox에서 bind 뒤에 오므로 connector가 pipe를 admit·drain하고 종료 edge를
   관찰한다. 이전 순서에서는 pipe_term이 먼저 도착해 connector의 `process_bind`→`attach_pipe`가
   비활성 pipe를 거부했고, 거부된 pipe는 preamble 하나를 물고 `waiting_for_delimiter`로 close까지 방치되어
   §4의 "connector는 그 pipe의 종료를 monitor로 관찰한다"가 성립하지 않았다.
   `send_bind` 실패 분기는 앞서 잡은 bind pipe lifetime ref를 되돌린 뒤 기존 정리(시퀀스 예약 balance, 양쪽
   terminate)를 그대로 수행한다.

`write_routing_id_and_flush`를 no-op으로 바꾸는 대안은 택하지 않았다. §4는 routing-id preamble을 종료된
pipe에 쓰는 동작을 정의하지 않으며, 그렇게 하면 "누가 언제 preamble을 쓰는가"에 두 번째 규칙(종료 뒤에는
무시)이 생긴다. 순서 정렬은 규칙을 하나로 줄인다.

**수정 전/후 규칙 수:** inproc 연결 완료 순서 2→1
(직접 경로: preamble → connector admit → binder admit / pending 경로: binder admit → connector admit →
bind preamble ⇒ 모든 경로: 두 preamble → connector admit queue → binder admit). 새 상태·flag·helper·옵션 없음.

## 소유권·spec·parity·분류

- **소유 계층:** Core inproc 연결 완료 소유자 `ctx_inproc_registry_t::connect_inproc_sockets`. ROUTER REJECT
  close(`router_admission.cpp`)와 PAIR 단일 peer 규칙(`pair.cpp`), pipe 종료 handshake(`pipe.cpp`, D-090 순서)는
  변경하지 않았다.
- **Spec 조항:** `core/doc/spec/core/socket/README.ko.md` §4 rid 중복 정책 — "등록하지 않은 중복 pipe는 즉시 닫는다.
  따라서 connector는 그 pipe의 종료를 monitor로 관찰하고 connect intent에 따라 다시 연결" (수정 2가 pending 경로에서
  이 문장을 성립시킨다); §6 `zlink_connect`("socket은 여러 endpoint에 연결할 수 있으며 … 자동으로 재연결"),
  `zlink_close`/`zlink_disconnect`(pending 자원 정리). spec 변경 없음.
- **교차언어 parity:** n/a(Core). binding은 abort 대신 정상 close/bind를 관찰한다. java framework 코드는 읽기만 했다.
- **변경 분류:** B — 기존 Core 결함 수정(D-B112가 노출). 계약 변경·상위 계층 보상 없음.

## 변경 파일 (작업 트리)

- `core/src/runtime/core/ctx_inproc_registry.cpp` — 위 수정 1·2.
- `core/tests/integration/test_inproc_pending_connect_rejected_at_attach.cpp` — 신규 통합 테스트(공개 C API만).
  4 케이스: `test_two_pending_connects_then_close`(최소 재현: ROUTER가 미bind inproc endpoint에 두 번 connect 후 close),
  `test_two_pending_connects_then_disconnect`(같은 상태에서 `zlink_disconnect`),
  `test_two_pending_same_rid_then_reject_bind`(같은 RID의 pending 2개 뒤 REJECT ROUTER bind: 첫 연결 admit·request
  round-trip 2회, 두 번째 connector는 DISCONNECTED edge 관찰),
  `test_mesh_peer_replacement_sequence`(java 시퀀스: peer close → 같은 RID replacement bind → 재connect → replacement
  close → 종료 edge 2개 수집 → local close). 미수정 트리에서 4/4 abort, 수정 트리에서 4/4 PASS.
  monitor는 tearDown에서 모두 닫아 실패 시 ctx term이 걸리지 않게 했다.
- `core/tests/CMakeLists.txt` — 등록 + TIMEOUT 60.

## 검증 결과

| 항목 | 명령/조건 | 결과 |
|---|---|---|
| 재현(미수정 라이브러리) | 최종 test 파일, `ctx_inproc_registry.cpp`만 HEAD로 되돌려 재빌드 | 4/4 케이스 `Assertion failed: written (pipe.cpp:210)` |
| 새 test | `ctest -R '^test_inproc_pending_connect_rejected_at_attach$' --repeat until-fail:10` | 10/10 PASS (0.06 s/회) |
| 새 test 부하 | mesh 케이스 단독 20회, `yes` 4개로 CPU 부하 | 20/20 PASS |
| 타깃 regex | `test_router_reject_duplicate\|test_socket_disconnect_boundary\|test_router_reciprocal_handover_lanes\|test_ctx_term_fixed_rid_handover\|test_phase3_request_reply_contract\|test_inproc` `-j2` | 6/6 PASS (48.0 s) |
| 전체 ctest | `ctest -j2` (core/build-dev, dev 빌드) | 174/175 PASS, 243.4 s; 유일한 실패 `hotpath_gate`(dev 빌드 n/a, 아래). 새 test 포함 |
| java M6A 클래스 | `ZLINK_LIBRARY_PATH=<worktree>/core/build-dev/lib/libzlink.so.0.17.0 ./gradlew :zlink-framework-core:test --tests '*ZLinkJavaRawMeshNodeM6ATest*'` ×4 | abort 없음; 28 중 27 PASS, `descriptorFenceReplacesEndpointOnlyIntent` 4/4 FAIL(아래) |
| java 유발 케이스 | 같은 조건, `observedInprocCloseDoesNotFenceDescriptorReplacement` | PASS (0.145 s) |
| `git diff --check` | | 진단 없음 |

`hotpath_gate`는 dev 빌드에서 네 cell 모두 ratio ≈1.26~1.32로 FAIL하며 기준값이 release+LTO용이라
brief대로 n/a로 둔다(변경은 send hot path를 건드리지 않는다; 이전 full ctest 보고도 hotpath n/a).

## BLOCKERS / 미해결

1. **`ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent` (java:601)** — 수정 라이브러리로 클래스
   전체를 돌리면 4/4 실패("Expected IllegalStateException … nothing was thrown": fenced intent가 이미 closed).
   단독 실행은 패키지(미수정) 라이브러리·수정 라이브러리 모두 PASS. 미수정 라이브러리로는 클래스 전체가
   abort하므로(유발 케이스를 빼고 27개만 돌려도 다른 케이스가 같은 assert로 abort) 기준 비교가 불가능하다.
   가설: D-B112의 REJECT 즉시 close로 생긴 재연결 루프(4단계; local이 같은 endpoint 재연결로 자기 admitted pipe를
   대체·종료하고 새 pipe는 peer가 REJECT)에서 fenced 연결이 CLOSED로 귀속되어 fence 검사가 throw하지 않는다.
   이는 D-088(REJECT ROUTER same-RID 재연결 의미, spec gap·사용자 결정)의 영역이며 이 job에서는 건드리지 않았다.
   Framework 변경도 하지 않았다. 감독 판단 필요.
2. **ROUTER connector가 REJECT된 pipe의 DISCONNECTED를 보려면 application recv가 필요하다.** connector 쪽 pipe에는
   bind 쪽 routing-id preamble이 미읽음으로 남고 REJECT close는 `terminate (true)`(delay)라서 pipe가
   `waiting_for_delimiter`에서 app recv(ROUTER recv는 routing-id frame을 건너뛰며 drain)까지 기다린다.
   DEALER connector(preamble 없음)는 즉시 관찰한다. java node는 poller로 항상 pump하므로 영향이 없고 새 test도 같은
   pump 패턴을 쓴다. "app poll 없이 진행"(D-B104)의 범위에 이 경우가 들어가는지는 spec 결정 사항으로 보고만 한다.
3. hotpath_gate는 dev 빌드에서 n/a(위). release+LTO 측정은 하지 않았다.
