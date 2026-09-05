# B 세션 작업 요청 — 캠페인 A(0.17.0 DONTWAIT framework completion)에서 넘기는 Core/bindings 작업 6건

너는 머신 B의 감독 세션이다. 규칙은 그대로다: 모든 조사·구현은 codex sub-agent(sol/high 기본, 어려우면 sol/xhigh,
기계적 게이트는 terra/medium)로 진행하고 너는 브리프 작성·게이트 실행·diff 리뷰·커밋/푸시만 한다. sub-agent는
절대 spec(`core/doc/spec/**`, `bindings/doc/spec/**`, `framework/doc/framework/**`, `doc/site/**`)을 수정하지 않는다.
리뷰 때 spec gap을 만들지 않았는지 확인한다. 버그로 확정되면 회귀 테스트 + 수정 + main 커밋·푸시까지 한다.
Core 테스트는 공개 C API만 사용하고 `ctest -j2`(CONTRIBUTING.ko.md §4/§5). 작업은 main에서 하고, A 세션도 main에
커밋하므로 push 전에 항상 `git fetch && git merge --no-edit origin/main`. `framework/**`는 건드리지 않는다(A 영역).

## 배경 — 왜 이 6건인가
A 캠페인은 게이트를 닫았지만, 전수 리뷰(`doc/plan/c016-worklog/review-campaign-{dotnet,java,cpp-node}.md`,
`review-layer-ownership-audit.md`)에서 dotnet/java framework가 Core/binding 책임을 상위에서 재구현한 보상 코드가
확인됐다(dotnet Mesh의 reciprocal HANDOVER settlement·RID→pair re-pin·reply epoch, dotnet/java ClientServer의 수동
reconnect + 두 번째 poller, java의 합성 connection id·errno 표). A는 그 코드를 지울 예정인데, **지우기 전에 하위 계층이
spec대로 동작하는지 Core/binding 테스트로 먼저 확정**해야 한다(감사 §10 "먼저 신뢰해야 할 계약과 하위 test", §11
"삭제 전 root에서 고칠 항목"). 3~6번이 그 전제조건이고, 1~2번은 앞서 전달한 후속이다.

각 작업의 결론은 `doc/plan/c016-worklog/decisions.ko.md`에 D-B9x로 기록하고, **A가 삭제 작업에 쓰도록 "spec대로 동작 확인
(테스트 이름)" 또는 "Core/binding 버그 → 수정 커밋 해시"를 한 줄로 명시**한다.

---

## 작업 3 (P0) — Core §4 reciprocal HANDOVER: 패배 방향 두 lane의 종료를 계약 테스트로 확정
근거: `core/doc/spec/core/socket/README.ko.md` §4 RID duplicate policy(HANDOVER, reciprocal은 RID 비교로 한 방향),
`07-router.ko.md`. 감사 §10 순서 1, §11 항목 1.
- 시나리오(공개 C API, tcp + inproc): ROUTER A·B가 서로 fixed RID로 connect(reciprocal). Application lane 0 +
  Completion lane 1(READY `Zlink-Lane`) 두 lane 세트가 양방향으로 생김.
- 확정할 것: (a) RID 비교로 정해진 승자 방향만 남고 **패배 방향의 두 lane이 실제로 종료**되어 양쪽 socket monitor에
  Disconnected/Closed로 관찰되는지, 언제(즉시 / linger / 타이머?); (b) 양쪽 노드의 active direction이 동일한지;
  (c) 패배 방향에 이미 admit된 REQUEST는 timeout 1회로 끝나고, 재전송은 승자 방향으로 성공하는지;
  (d) 승자 방향의 첫 REQUEST가 패배 lane 종료 전에 제출돼도 정상 완료되는지(= dotnet이 "settlement 전 submit"을
  두려워한 그 경우).
- 의심 지점: `core/src/runtime/sockets/router/router_admission.cpp` `identify_peer`의 `reciprocal_duplicate` 경로가
  loser를 **anonymous standby pipe로 유지**한다(dotnet 주석의 근거). standby가 남아 lane이 종료되지 않으면 §4 위반 →
  Core 수정. standby가 spec 의도라면 spec 조항을 인용해 "종료되지 않는다"를 명시(그 경우 A는 settlement 코드를 삭제하고
  Core survivor를 그대로 쓴다).
- 산출: `core/tests/integration/test_router_reciprocal_handover_lanes.cpp`(labels integration;serial), 결과표(tcp/inproc ×
  reconnect interval), 수정 시 회귀 포함.

## 작업 4 (P0) — Core polling §3 command progress: disconnect terminal·자동 reconnect가 application poll 없이 진행되는지
근거: `core/doc/spec/core/05-polling.ko.md` §3(lost-wake/progress), `socket/README.ko.md` connect/disconnect/reconnect intent.
감사 §10 순서 3, §11 항목 2.
- 시나리오(공개 C API): DEALER가 ROUTER endpoint에 connect·admit된 뒤 (a) 서버가 pipe를 끊었을 때 application이 poll하지
  않아도 Disconnected와 자동 reconnect READY가 오는지(public poller에 등록만 하고 wait 중 / wait 안 하는 두 경우);
  (b) `zlink_disconnect(endpoint)` 직후 같은 socket에서 `zlink_connect(endpoint)`를 다시 호출했을 때 terminal edge 전에
  호출해도 새 pipe로 admission이 가는지, 아니면 이전 pipe가 다시 선택되는지(그렇다면 spec의 어떤 조항이 그것을 허용/금지하는가);
  (c) (b)에서 REQUEST를 제출하면 어느 pipe에 큐잉되는지.
- 배경: dotnet ClientServer(`ZLinkClientServerClientRuntime.cs` `ReconnectAsync`)는 Disconnected를 기다리는 동안
  **같은 socket에 두 번째 POLLIN poller**를 만들어야 disconnect command가 진행됐다고 보고했다(round-2 trace:
  "Disconnect 뒤 약 5초 동안 terminal edge가 진행되지 않음"). Core가 spec대로 진행한다면 그 poller와 수동 reconnect
  상태기계를 A가 삭제한다. 진행이 안 된다면 Core lost-wake 버그 → 수정.
- 산출: `core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp`, 결과표, 수정 시 회귀.

## 작업 5 (P1) — bindings/java monitor pair identity와 monitor drain 소유권
근거: `bindings/doc/spec/java/README.ko.md`(monitor·poller 절), `core/doc/spec/core/06-monitoring*.ko.md`(connection_id,
lane). 감사 §10 순서 2, §11 항목 3; java 리뷰 "스펙 gap 후보 1·2".
- 확정할 것: READY/DISCONNECTED/CLOSED가 **같은 nonzero `(connectionId, lane)`**을 tcp/inproc 모두에서 주는지 binding
  테스트로 고정. java `Poller`(`bindings/java/src/main/java/systems/zlink/contracts/eventing/Poller.java:12-34`)가
  `SocketMonitor`를 받지 못해 monitor drain 소유권이 없는 문제를 dotnet/cpp와 동등한 표면으로 해결(spec에 없는 표면이면
  spec gap으로 보고하고 사용자 결정 요청 — sub-agent가 spec을 고치게 하지 말 것).
- 배경: java framework(`ZLinkJavaRawMeshNode.java`)는 identity가 없어 UUID·endpoint FIFO로 READY와 DISCONNECTED를 짝지었고,
  0.16 전환 커밋 `e65abaf7ac`가 replacement fence를 제거하면서 `ZLinkJavaRawMeshNodeM6ATest` 2건(`:1400`, `:1439`)이 회귀했다.
  binding이 identity를 주면 A가 합성 코드를 지우고 M6A를 복원한다.
- 산출: bindings/java 테스트(pair identity 3 event 동일성, monitor를 poller에 등록해 drain), 필요 시 binding 수정 커밋.

## 작업 6 (P1) — bindings/java portable typed error: route loss vs admission 거절을 errno 없이 구분
근거: `bindings/doc/spec/**` error policy(java 리뷰가 인용한 3972-3977, 4234-4252 부근), `core/doc/spec/core/03-errors.ko.md`,
`socket/README.ko.md` submit result 표(NONE vs DONTWAIT: unknown RID = `NOT_FOUND+ENOENT` / `NOT_CONNECTED+EHOSTUNREACH`,
wait-token terminal `ENOENT`). 감사 §10 순서 5, §11 항목 4.
- 확정할 것: java async 경로에서 exact-route loss(`NOT_CONNECTED`), admission 거절(`NOT_ADMITTED`), capacity
  (`BACKPRESSURED`+wait token), wait-token terminal(`ENOENT`)이 **raw errno 숫자 없이 서로 다른 typed result**로 나오는지.
  coarse `NOT_ADMITTED` 하나로만 나오면 binding 버그 → 수정.
- 배경: java framework는 `ZLinkJavaStreamSocket.isRemoteRouteUnavailable()`(`:824-835`) 등에서 `NOT_ADMITTED` 전체를
  허용하거나 errno를 두 곳에서 재분류한다. binding이 typed result를 주면 A가 errno 표를 지운다.
- 산출: bindings/java 테스트(4 case typed result), 필요 시 binding 수정 커밋.

## 작업 1 (P1) — D-086: tcp에서 same-RID replacement DEALER admission 지연 (Core, 성능)
증상: ROUTER에 fixed RID로 admitted된 DEALER A의 pipe가 살아있는 상태에서 같은 RID의 DEALER B가 **tcp**로
connect하면(HANDOVER) B의 admission 완료까지 0.1~2.9 s, 간헐 5 s 이상. A의 reconnect interval이 짧을수록 악화. **inproc은 즉시**.
repro: `doc/plan/c016-worklog/evidence/test_ctx_term_fixed_rid_handover.cpp`(+ `…cmake.diff`), 맥락
`core-ctx-term-teardown-hang-summary.md` §"Secondary finding". 영향: .NET handover 테스트의 2 s 기대치가 이 분포 안에 있음
(`HandoverKeepsPriorReplyEpochUntilExactDisconnect :560`, 36분짜리 D-068 sibling).
- 할 일: repro를 `core/tests/integration/`에 등록(labels integration;serial, TIMEOUT 30), 지연 분포 측정(tcp/inproc ×
  reconnect interval 10/100/1000 ms × 20회). 원인을 `router_admission.cpp` handover 경로·tcp peer 종료 handshake·linger·reaper
  타이밍에서 찾고 spec(§4, `07-router`)의 handover 완료 조건과 대조. spec이 "이전 pipe 종료 확인 후 handover"를 요구하면
  지연 자체는 계약일 수 있음 — 그 경우 상한과 원인을 문서화하고 계약 안에서 줄일 부분만 수정. 수정 시 회귀(예: tcp handover
  admission < 200 ms p95) + CONTRIBUTING §5 게이트. 작업 3의 테스트와 시나리오를 공유하므로 같은 sub-agent가 이어서 해도 된다.

## 작업 2 (P2) — D-087: bindings/java 네이티브 라이브러리 추출 임시 디렉터리 누수
위치: `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/LibraryLoader.java:60-64`. JVM마다
`Files.createTempDirectory("zlink-native-")`에 `libzlink.so`(84 MB)를 복사하고 `deleteOnExit`에만 의존. role JVM을
SIGKILL하는 샘플 runner에서 삭제되지 않아 머신 A에서 하룻밤 java 게이트로 약 95개(≈8 GB)가 남아 8 GB tmpfs `/tmp`가 100%.
- 할 일: 추출 위치를 `${ZLINK_JAVA_NATIVE_CACHE:-~/.cache/zlink/native}/<sha256-of-resource>/<libFile>`로 바꾸고 같은
  해시가 있으면 복사 생략 후 `System.load`. 동시 JVM 경합은 임시 파일 + atomic move. 캐시 디렉터리 생성 실패 시 기존 temp
  방식 fallback. `preloadWindowsDeps` 경로도 같은 디렉터리 유지. 회귀: JVM 2회 연속 로드 시 temp 디렉터리 증가 0, 캐시
  파일 1개, 해시 불일치 시 재추출. `bindings/doc/spec/**`에 로더 경로 계약이 있으면 네가 확인(계약 변경 필요 시 사용자 보고).

---

## 산출물과 순서
- 순서: 3 → 4 → 5 → 6 → 1 → 2 (3·4는 A의 dotnet 삭제 작업을 막고 있으니 먼저).
- 각 작업마다 `doc/plan/c016-worklog/briefs/<job>.prompt`와 `…-summary.md`(원인 `file:line`, spec 조항, 수정, 회귀 테스트,
  게이트 수치, BLOCKERS). 커밋 메시지에 D-086/D-087 또는 리뷰 항목 참조.
- 완료 후 A가 로컬 패키지를 재빌드해 framework 게이트를 다시 돌릴 수 있게 **마지막 커밋 해시를 decisions에 남긴다.**
- 작업 3~6에서 "spec에 명시가 없다"가 결론이면 Core/binding을 임의로 바꾸지 말고 spec gap으로 정리해 사용자 결정을 요청한다.

---

## 작업 7 (P0, 2026-09-05 10:20 추가) — Core: HANDOVER로 물러난 pair에 고정된 REQUEST를 즉시 `ZLINK_REQUEST_NOT_CONNECTED`로 종결
근거: spec 개정 `core/doc/spec/core/socket/README.ko.md` §4(같은 커밋, en 포함). 이전 문구 "request는 자기 timeout으로 정확히 한 번 종결"을
"pair가 handover로 물러나는 즉시 `ZLINK_REQUEST_NOT_CONNECTED`(errno `EHOSTUNREACH`)로 정확히 한 번 종결, 자기 timeout까지 기다리지 않음"으로
바꿨다. 이유: framework durable operation replay(04-actor-model sender 규칙)는 attempt마다 남은 deadline 전부를 request timeout으로 넘기므로,
Core가 자기 timeout까지 기다리면 replay할 시간이 남지 않는다(cpp astra job 보고 `stage2-cpp-replay-scope-summary.md`). 재시도 예산 분할은 금지 패턴.
- 구현: `router_admission.cpp` reciprocal collapse / same-direction handover에서 물러나는 pair를 결정하는 지점에서, 그 pair에 pinned된 pending REQUEST를
  `REQUEST_NOT_CONNECTED`로 완료(reply pinning 규칙 `socket_request_reply_submit_api.cpp:121-180`의 pair/generation fence는 유지). standby pipe 자체는
  D-B96대로 유지.
- 테스트: `core/tests/integration/test_router_reciprocal_handover_lanes.cpp:395-434`의 losing request 기대를 `REQUEST_TIMED_OUT` → `REQUEST_NOT_CONNECTED`로
  바꾸고, completion이 losing request timeout보다 훨씬 이른 시점(예: < 100 ms, timeout 2 s)임을 assert. same-direction HANDOVER(같은 방향 재연결)도 같은 케이스 추가.
- 완료 후 decisions에 커밋 해시 한 줄. A는 그 뒤 cpp/java/node/dotnet의 replay 범위(handover 행)를 마무리한다.

