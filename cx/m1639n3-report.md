# m1639n3 — Node request 경로 계수 회귀 수정

결과: 미사용 회전 permit의 개별 반환을 host queue owner의 일괄 반환으로 바꿨습니다. 단일 request 수신 회전의 pressure 판정은 2e785d8963^ 4회 / 2e785d8963 65회 / 수정 후 4회입니다. Promise·Buffer 감소와 예약 예산 64건, FIFO·정확히 한 번 반환·사용 중 저장소 수명은 유지했습니다. 처리량은 측정하지 않았으며 −15~−19% 저하가 해소됐다는 처리량 판정은 하지 않습니다.

## Rebase

Windows worktree `D:/worktree/zlink-1639-msg-node`, branch `framework-node/1639-per-message-cost`에서 시작 시 clean을 확인했습니다. `git fetch origin` 뒤 `origin/main=37cca3ab1516b488dcb43f101d1d32e782aa21bb` 위로 6개 커밋을 rebase했습니다. 충돌 0건이며 충돌 해결용 별도 커밋은 없습니다. 마지막 기존 커밋은 `2e785d8963` → `e1ed7745ee0fb7fc952ce929c30e797bf990c83a`입니다.

최초 rebase gate는 의존성 준비 오류와 main의 Unity package 빌드 결함으로 유효한 전체 결과를 얻지 못했습니다. 의존성 오류를 제거한 main/최종 전체 검사에 이어, 수정 이전에 보존한 rebase snapshot을 독립 사본에 풀어 runtime·m6a/m6b/m6c·format 전체를 검증했습니다. 따라서 rebase만의 새 실패 0을 따로 확인했지만, 최초 전체 검증 완료 전에 구현이 진행된 절차 차이는 남깁니다.

## 원인과 결정 소유

원인: 마지막 커밋의 `host/application-job-queue.ts:236` 미사용 grants 루프는 각 permit의 `releaseAfterInternalProcessing()`를 호출하고, 기존 `:481` queue release는 매번 `drainWaiters()`와 `evaluatePressure()`를 호출했습니다. `foundation/raw-service-mesh-runtime.ts:688–711`은 회전 예산 64를 예약하므로 한 건만 받은 회전에서도 미사용 예약을 건별 반환했습니다. Promise가 줄어든 사실과 이 동기 판정 반복은 서로 다른 계수입니다.

수정: 현재 `host/application-job-queue.ts:482`에서 미사용 저장소의 기존 terminal 전환·재사용을 처리하고, `:498`의 단일 capacity 반환 owner로 개수를 전달합니다. FIFO grant와 pressure 판정은 queue owner에서 한 번 실행합니다. 정상 permit과 batch의 terminal 전환은 `:180` 한 곳을 재사용합니다. handler 진입 전 반환·ingress 저장소 수명·회전 예산은 변경하지 않습니다.

대안: F의 회전 예약을 이전 건별 acquire로 되돌리면 hot-path I1의 회전 예산 확보와 send 개선을 함께 잃습니다. 기존 owner의 일괄 반환을 선택했습니다. 정리 future에 request별 새 Promise를 추가한 경로는 측정에서 찾지 못했으며, 비교한 request 단계의 Promise 수는 감소했습니다. 불필요한 retry·timer·queue·lock·poller·generation·flag 추가는 없습니다.

| 결정 | 소유 spec 조항 | 현재 구현 대조 | 단일 owner |
|---|---|---|---|
| reservation 예산·count 반환·FIFO·pressure | `04-application-job-queue-and-backpressure.ko.md:121–130,230–237,327` / `08-messaging-hot-path.ko.md:178` | Node `host/application-job-queue.ts:180,477,482,498`; .NET `ZLinkApplicationJobQueue.cs:367,475,502,524`; Java `ZLinkApplicationJobQueue.java:594,617,643`; C++ `application_job_queue.hpp:176,207` | 각 언어 host queue owner |
| handler turn·terminal 진행 | `02-handler-turn-and-execution-gate.ko.md:76–104,137–149` | 기존 gate와 async continuation 그대로; 이번 수정은 Node queue의 동기 count 반환만 변경 | 기존 execution owner |

소유 계층: Framework host의 Application Job Queue가 reservation·terminal count·FIFO grant·pressure를 소유하고 Core/binding의 물리 IO 결정을 재구현하지 않습니다.
Spec 조항: server `01-execution/04-application-job-queue-and-backpressure.ko.md` §3·§6(:121–130,230–237,327), `08-messaging-hot-path.ko.md` I1(:178), `02-handler-turn-and-execution-gate.ko.md` §2·§4(:76,137)입니다.
교차언어 대조: C++·Java·.NET 모두 기존 queue owner에서 permit phase/count 반환과 FIFO·pressure를 결정합니다. .NET ReleaseBatch도 기존 ReleaseOnLane을 재사용합니다. Node의 동일 JS owner turn 안 표현을 일괄 반환으로 줄였으며 공개 동작 차이나 새 계약은 추가하지 않았습니다. 다른 언어 파일은 수정하지 않았습니다.
변경 분류: B — 기존 queue 계약의 반환 표현 비용과 중복 accounting 검사 제거입니다. C/D 또는 스펙 수정은 없습니다.

## 계수 측정

Node 22.23.2, pinned binding/Core package 1.19.0, payload 1024B입니다. public NestJS host 두 개와 기존 benchmark transport를 사용했습니다. 연결 준비 뒤 request 16건으로 초기화를 마치고, 각 pattern을 정확히 128건 실행했습니다. backpressure pattern은 기존 request window 형태의 8개 동시 request이며 강제 native EAGAIN 주입 실험은 아닙니다. 모두 received=128입니다. source/target이 같은 process에 있으므로 공통 raw·pump module은 두 역할을 합산합니다. Socket handle이 확인되는 native 호출만 역할을 붙이고 frame handle 소유를 확인할 수 없는 호출은 shared로 남깁니다.

PROMISE는 async_hooks init, callback은 before 수입니다. Immediate/Timeout/Microtask는 JS 명시적 resource 생성과 실행만 셉니다. native readiness callback 전체나 OS context switch 수로 해석하지 않습니다. Promise callback 수는 continuation 비용의 지표이며 문법상의 await 깊이와 같지 않습니다. Buffer-bytes는 factory가 반환한 길이 합으로 view도 포함하므로 실제 할당 바이트로 해석하지 않습니다.

JS 할당은 별도 실행에서 fixed 2048건, HeapProfiler interval 128B, minor/major GC로 수거된 객체 포함으로 sampling했습니다. Framework runtime·binding frame의 selfSize만 집계한 추정치입니다. native malloc과 application/serializer 전체를 합친 실제 할당량은 아닙니다. heap sampling 실행의 Promise 계수는 사용하지 않습니다. readiness 회전 수·sampling 추정값의 작은 변화는 시스템 부하 영향을 받으므로 primary 판정은 별도 단일 회전 재현의 4/65/4입니다.

| pattern | 계수/메시지 | 2e785d8963^ | 2e785d8963 | 수정 후 |
|---|---|---:|---:|---:|
| request-serial | PROMISE | 98.273 | 50.203 | 50.242 |
| request-serial | PROMISE-callback | 50.000 | 25.898 | 25.938 |
| request-serial | Immediate | 0.008 | 0.008 | 0.008 |
| request-serial | Immediate-callback | 0.008 | 0.008 | 0.008 |
| request-serial | Timeout | 3.320 | 3.242 | 3.250 |
| request-serial | Timeout-callback | 3.305 | 3.219 | 3.234 |
| request-serial | Microtask | 0.000 | 0.000 | 0.000 |
| request-serial | Buffer | 49.000 | 46.000 | 46.000 |
| request-serial | Buffer-bytes | 20094.000 | 19043.000 | 19043.000 |
| request-serial | runtime/binding sampled JS heap B | 43324.336 | 35064.422 | 30645.090 |
| request-serial | source/permit pressure | 1.047 | 37.578 | 1.734 |
| request-serial | source/permit waiter drain | 0.523 | 37.000 | 1.156 |
| request-serial | target/permit pressure | 5.047 | 102.070 | 5.711 |
| request-serial | target/permit waiter drain | 2.523 | 100.500 | 4.141 |
| request-backpressure | PROMISE | 77.656 | 31.078 | 31.078 |
| request-backpressure | PROMISE-callback | 42.609 | 18.273 | 18.281 |
| request-backpressure | Immediate | 0.133 | 0.008 | 0.008 |
| request-backpressure | Immediate-callback | 0.133 | 0.008 | 0.008 |
| request-backpressure | Timeout | 0.875 | 0.469 | 0.469 |
| request-backpressure | Timeout-callback | 0.852 | 0.445 | 0.445 |
| request-backpressure | Microtask | 0.000 | 0.000 | 0.000 |
| request-backpressure | Buffer | 49.000 | 46.000 | 46.000 |
| request-backpressure | Buffer-bytes | 20094.000 | 19043.000 | 19043.000 |
| request-backpressure | runtime/binding sampled JS heap B | 31845.945 | 19819.719 | 19742.039 |
| request-backpressure | source/permit pressure | 0.016 | 3.047 | 0.141 |
| request-backpressure | source/permit waiter drain | 0.008 | 3.000 | 0.094 |
| request-backpressure | target/permit pressure | 2.375 | 11.172 | 1.516 |
| request-backpressure | target/permit waiter drain | 1.188 | 11.000 | 1.344 |
| send | PROMISE | 68.195 | 12.742 | 12.742 |
| send | PROMISE-callback | 38.508 | 8.320 | 8.328 |
| send | Immediate | 0.352 | 0.070 | 0.070 |
| send | Immediate-callback | 0.352 | 0.070 | 0.070 |
| send | Timeout | 0.648 | 0.086 | 0.086 |
| send | Timeout-callback | 0.633 | 0.070 | 0.070 |
| send | Microtask | 0.000 | 0.000 | 0.000 |
| send | Buffer | 20.000 | 16.000 | 16.000 |
| send | Buffer-bytes | 11031.000 | 9975.000 | 9975.000 |
| send | runtime/binding sampled JS heap B | 23297.477 | 9315.656 | 9233.898 |
| send | source/permit pressure | 0.016 | 0.508 | 0.023 |
| send | source/permit waiter drain | 0.008 | 0.500 | 0.016 |
| send | target/permit pressure | 2.016 | 2.031 | 1.070 |
| send | target/permit waiter drain | 1.008 | 2.000 | 1.039 |

단계별 표는 첫 runtime/binding stack frame 기준입니다. raw backend와 pump는 ingress·completion·idle 관리 모두를 포함하므로 두 역할 합계입니다. 같은 파일의 Promise와 heap은 계측 모드가 다르며 서로 더하지 않습니다. request는 준비→submit→binding completion→public reply, target은 ingress→dispatch→reply→permit 정리 순서로 읽습니다.

### request-serial 단계별

| 단계 / frame | 지표/메시지 | parent | last | final |
|---|---|---:|---:|---:|
| source submit / reply delivery / `channel-transports.js` | PROMISE | 4.000 | 4.000 | 4.000 |
| source submit / reply delivery / `channel-transports.js` | PROMISE-callback | 2.000 | 2.000 | 2.000 |
| source submit / reply delivery / `channel-transports.js` | sampled heap B | 1827.5 | 1796.3 | 1793.1 |
| source public admission / reply / `request-result.js` | PROMISE | 3.000 | 1.000 | 1.000 |
| source public admission / reply / `request-result.js` | PROMISE-callback | 2.000 | 1.000 | 1.000 |
| source public admission / reply / `request-result.js` | sampled heap B | 411.0 | 908.2 | 894.0 |
| source completion storage / `mesh-completion-table.js` | PROMISE | 1.000 | 1.000 | 1.000 |
| source completion storage / `mesh-completion-table.js` | PROMISE-callback | 0.000 | 0.000 | 0.000 |
| source completion storage / `mesh-completion-table.js` | sampled heap B | 396.2 | 158.3 | 164.0 |
| source logical operation / `operation-registry.js` | PROMISE | 1.000 | 1.000 | 1.000 |
| source logical operation / `operation-registry.js` | PROMISE-callback | 0.000 | 0.000 | 0.000 |
| source logical operation / `operation-registry.js` | sampled heap B | 459.7 | 451.9 | 437.0 |
| binding request bridge / receive / `node-raw-binding-port.js` | PROMISE | 2.000 | 2.000 | 2.000 |
| binding request bridge / receive / `node-raw-binding-port.js` | PROMISE-callback | 1.000 | 1.000 | 1.000 |
| binding request bridge / receive / `node-raw-binding-port.js` | sampled heap B | 2048.7 | 1573.5 | 1598.9 |
| binding future / admission / `completion_owner.js` | PROMISE | 4.000 | 4.000 | 4.000 |
| binding future / admission / `completion_owner.js` | PROMISE-callback | 2.000 | 2.000 | 2.000 |
| binding future / admission / `completion_owner.js` | sampled heap B | 510.8 | 511.7 | 530.6 |
| raw protocol / ingress (both) / `raw-service-mesh-runtime.js` | PROMISE | 18.922 | 5.852 | 5.867 |
| raw protocol / ingress (both) / `raw-service-mesh-runtime.js` | PROMISE-callback | 7.047 | 1.000 | 1.000 |
| raw protocol / ingress (both) / `raw-service-mesh-runtime.js` | sampled heap B | 9741.9 | 6156.0 | 6410.4 |
| backend / completion / poll (both) / `node-raw-mesh-backend.js` | PROMISE | 10.562 | 10.703 | 10.734 |
| backend / completion / poll (both) / `node-raw-mesh-backend.js` | PROMISE-callback | 5.781 | 5.852 | 5.867 |
| backend / completion / poll (both) / `node-raw-mesh-backend.js` | sampled heap B | 4856.8 | 4736.8 | 4869.3 |
| worker / terminal (both) / `mesh-dispatch-pump.js` | PROMISE | 12.758 | 9.617 | 9.609 |
| worker / terminal (both) / `mesh-dispatch-pump.js` | PROMISE-callback | 7.156 | 6.031 | 6.055 |
| worker / terminal (both) / `mesh-dispatch-pump.js` | sampled heap B | 3351.0 | 2686.6 | 2709.6 |
| target handler / reply / `channel-dispatch-pipeline.js` | PROMISE | 5.000 | 2.000 | 2.000 |
| target handler / reply / `channel-dispatch-pipeline.js` | PROMISE-callback | 3.000 | 1.000 | 1.000 |
| target handler / reply / `channel-dispatch-pipeline.js` | sampled heap B | 1596.5 | 671.7 | 672.2 |
| target activation / `channel-dispatch-services.js` | PROMISE | 4.000 | 1.000 | 1.000 |
| target activation / `channel-dispatch-services.js` | PROMISE-callback | 2.000 | 0.000 | 0.000 |
| target activation / `channel-dispatch-services.js` | sampled heap B | 616.1 | 47.8 | 49.8 |
| target host claim cleanup / `admission.js` | PROMISE | 2.000 | 1.000 | 1.000 |
| target host claim cleanup / `admission.js` | PROMISE-callback | 1.000 | 1.000 | 1.000 |
| target host claim cleanup / `admission.js` | sampled heap B | 678.9 | 342.2 | 339.8 |
| target permit cleanup / `application-job-queue-scope.js` | PROMISE | 3.000 | 1.000 | 1.000 |
| target permit cleanup / `application-job-queue-scope.js` | PROMISE-callback | 2.000 | 1.000 | 1.000 |
| target permit cleanup / `application-job-queue-scope.js` | sampled heap B | 893.1 | 0.0 | 0.0 |
| target node dispatch / `spot-node-runtime-manager.js` | PROMISE | 2.000 | 2.000 | 2.000 |
| target node dispatch / `spot-node-runtime-manager.js` | PROMISE-callback | 1.000 | 1.000 | 1.000 |
| target node dispatch / `spot-node-runtime-manager.js` | sampled heap B | 494.0 | 500.0 | 504.7 |
| source/native / `pollEventsFd` | native calls | 1.000 | 1.000 | 1.000 |
| source/native / `pollEventsRevents` | native calls | 1.000 | 1.000 | 1.000 |
| source/native / `pollEventsSlot` | native calls | 1.000 | 1.000 | 1.000 |
| source/native / `pollEventsSourceKind` | native calls | 1.000 | 1.000 | 1.000 |
| source/native / `pollerWaitInto` | native calls | 1.523 | 1.578 | 1.578 |
| source/native / `routerRecvMessageNoWait` | native calls | 0.523 | 0.578 | 0.578 |
| source/native / `socketCompletionRecv` | native calls | 2.000 | 2.000 | 2.000 |
| source/native / `socketSubmitRequest` | native calls | 1.000 | 1.000 | 1.000 |
| target/native / `pollEventsFd` | native calls | 0.992 | 0.992 | 0.992 |
| target/native / `pollEventsRevents` | native calls | 0.992 | 0.992 | 0.992 |
| target/native / `pollEventsSlot` | native calls | 0.992 | 0.992 | 0.992 |
| target/native / `pollEventsSourceKind` | native calls | 0.992 | 0.992 | 0.992 |
| target/native / `pollerWaitInto` | native calls | 1.523 | 1.570 | 1.570 |
| target/native / `routerRecvMessageNoWait` | native calls | 2.523 | 2.570 | 2.570 |
| target/native / `socketReply` | native calls | 1.000 | 1.000 | 1.000 |
| shared/native / `messageFrameClose` | native calls | 2.000 | 2.000 | 2.000 |
| shared/native / `messageFrameCopyData` | native calls | 2.000 | 2.000 | 2.000 |

### request-backpressure 단계별

| 단계 / frame | 지표/메시지 | parent | last | final |
|---|---|---:|---:|---:|
| source submit / reply delivery / `channel-transports.js` | PROMISE | 4.000 | 4.000 | 4.000 |
| source submit / reply delivery / `channel-transports.js` | PROMISE-callback | 2.000 | 2.000 | 2.000 |
| source submit / reply delivery / `channel-transports.js` | sampled heap B | 1836.0 | 1928.7 | 1902.5 |
| source public admission / reply / `request-result.js` | PROMISE | 3.000 | 1.000 | 1.000 |
| source public admission / reply / `request-result.js` | PROMISE-callback | 2.000 | 1.000 | 1.000 |
| source public admission / reply / `request-result.js` | sampled heap B | 195.3 | 421.2 | 435.6 |
| source completion storage / `mesh-completion-table.js` | PROMISE | 1.000 | 1.000 | 1.000 |
| source completion storage / `mesh-completion-table.js` | PROMISE-callback | 0.000 | 0.000 | 0.000 |
| source completion storage / `mesh-completion-table.js` | sampled heap B | 398.2 | 96.5 | 96.7 |
| source logical operation / `operation-registry.js` | PROMISE | 1.000 | 1.000 | 1.000 |
| source logical operation / `operation-registry.js` | PROMISE-callback | 0.000 | 0.000 | 0.000 |
| source logical operation / `operation-registry.js` | sampled heap B | 331.1 | 320.5 | 323.0 |
| binding request bridge / receive / `node-raw-binding-port.js` | PROMISE | 2.000 | 2.000 | 2.000 |
| binding request bridge / receive / `node-raw-binding-port.js` | PROMISE-callback | 1.000 | 1.000 | 1.000 |
| binding request bridge / receive / `node-raw-binding-port.js` | sampled heap B | 1999.7 | 1775.8 | 1803.0 |
| binding future / admission / `completion_owner.js` | PROMISE | 4.000 | 4.000 | 4.000 |
| binding future / admission / `completion_owner.js` | PROMISE-callback | 2.000 | 2.000 | 2.000 |
| binding future / admission / `completion_owner.js` | sampled heap B | 842.5 | 750.7 | 763.0 |
| raw protocol / ingress (both) / `raw-service-mesh-runtime.js` | PROMISE | 9.281 | 1.562 | 1.562 |
| raw protocol / ingress (both) / `raw-service-mesh-runtime.js` | PROMISE-callback | 5.195 | 1.000 | 1.000 |
| raw protocol / ingress (both) / `raw-service-mesh-runtime.js` | sampled heap B | 4138.9 | 1937.9 | 1938.9 |
| backend / completion / poll (both) / `node-raw-mesh-backend.js` | PROMISE | 2.516 | 2.125 | 2.125 |
| backend / completion / poll (both) / `node-raw-mesh-backend.js` | PROMISE-callback | 1.820 | 1.562 | 1.562 |
| backend / completion / poll (both) / `node-raw-mesh-backend.js` | sampled heap B | 2322.2 | 2224.1 | 2215.4 |
| worker / terminal (both) / `mesh-dispatch-pump.js` | PROMISE | 9.578 | 3.109 | 3.109 |
| worker / terminal (both) / `mesh-dispatch-pump.js` | PROMISE-callback | 5.453 | 2.570 | 2.578 |
| worker / terminal (both) / `mesh-dispatch-pump.js` | sampled heap B | 3272.8 | 695.0 | 673.5 |
| target handler / reply / `channel-dispatch-pipeline.js` | PROMISE | 5.000 | 2.000 | 2.000 |
| target handler / reply / `channel-dispatch-pipeline.js` | PROMISE-callback | 3.000 | 1.000 | 1.000 |
| target handler / reply / `channel-dispatch-pipeline.js` | sampled heap B | 1907.3 | 832.6 | 839.1 |
| target activation / `channel-dispatch-services.js` | PROMISE | 4.000 | 1.000 | 1.000 |
| target activation / `channel-dispatch-services.js` | PROMISE-callback | 2.000 | 0.000 | 0.000 |
| target activation / `channel-dispatch-services.js` | sampled heap B | 689.2 | 49.8 | 49.8 |
| target host claim cleanup / `admission.js` | PROMISE | 2.000 | 1.000 | 1.000 |
| target host claim cleanup / `admission.js` | PROMISE-callback | 1.000 | 1.000 | 1.000 |
| target host claim cleanup / `admission.js` | sampled heap B | 585.9 | 339.1 | 332.3 |
| target permit cleanup / `application-job-queue-scope.js` | PROMISE | 3.000 | 1.000 | 1.000 |
| target permit cleanup / `application-job-queue-scope.js` | PROMISE-callback | 2.000 | 1.000 | 1.000 |
| target permit cleanup / `application-job-queue-scope.js` | sampled heap B | 30.9 | 83.1 | 83.2 |
| target node dispatch / `spot-node-runtime-manager.js` | PROMISE | 2.000 | 2.000 | 2.000 |
| target node dispatch / `spot-node-runtime-manager.js` | PROMISE-callback | 1.000 | 1.000 | 1.000 |
| target node dispatch / `spot-node-runtime-manager.js` | sampled heap B | 312.8 | 493.1 | 487.7 |
| source/native / `pollEventsFd` | native calls | 0.180 | 0.125 | 0.125 |
| source/native / `pollEventsRevents` | native calls | 0.180 | 0.125 | 0.125 |
| source/native / `pollEventsSlot` | native calls | 0.180 | 0.125 | 0.125 |
| source/native / `pollEventsSourceKind` | native calls | 0.180 | 0.125 | 0.125 |
| source/native / `pollerWaitInto` | native calls | 0.188 | 0.172 | 0.172 |
| source/native / `routerRecvMessageNoWait` | native calls | 0.008 | 0.047 | 0.047 |
| source/native / `socketCompletionRecv` | native calls | 1.180 | 1.125 | 1.125 |
| source/native / `socketSubmitRequest` | native calls | 1.000 | 1.000 | 1.000 |
| target/native / `pollEventsFd` | native calls | 0.125 | 0.125 | 0.125 |
| target/native / `pollEventsRevents` | native calls | 0.125 | 0.125 | 0.125 |
| target/native / `pollEventsSlot` | native calls | 0.125 | 0.125 | 0.125 |
| target/native / `pollEventsSourceKind` | native calls | 0.125 | 0.125 | 0.125 |
| target/native / `pollerWaitInto` | native calls | 0.188 | 0.172 | 0.172 |
| target/native / `routerRecvMessageNoWait` | native calls | 1.188 | 1.172 | 1.172 |
| target/native / `socketReply` | native calls | 1.000 | 1.000 | 1.000 |
| shared/native / `messageFrameClose` | native calls | 2.000 | 2.000 | 2.000 |
| shared/native / `messageFrameCopyData` | native calls | 2.000 | 2.000 | 2.000 |

### send 단계별

| 단계 / frame | 지표/메시지 | parent | last | final |
|---|---|---:|---:|---:|
| source submit / reply delivery / `channel-transports.js` | PROMISE | 3.000 | 0.000 | 0.000 |
| source submit / reply delivery / `channel-transports.js` | PROMISE-callback | 2.000 | 0.000 | 0.000 |
| source submit / reply delivery / `channel-transports.js` | sampled heap B | 1322.3 | 338.1 | 343.9 |
| source public admission / reply / `request-result.js` | PROMISE | 0.000 | 0.000 | 0.000 |
| source public admission / reply / `request-result.js` | PROMISE-callback | 0.000 | 0.000 | 0.000 |
| source public admission / reply / `request-result.js` | sampled heap B | 0.0 | 0.0 | 0.0 |
| source completion storage / `mesh-completion-table.js` | PROMISE | 0.000 | 0.000 | 0.000 |
| source completion storage / `mesh-completion-table.js` | PROMISE-callback | 0.000 | 0.000 | 0.000 |
| source completion storage / `mesh-completion-table.js` | sampled heap B | 0.0 | 0.0 | 0.0 |
| source logical operation / `operation-registry.js` | PROMISE | 0.000 | 0.000 | 0.000 |
| source logical operation / `operation-registry.js` | PROMISE-callback | 0.000 | 0.000 | 0.000 |
| source logical operation / `operation-registry.js` | sampled heap B | 8.1 | 2.0 | 2.5 |
| binding request bridge / receive / `node-raw-binding-port.js` | PROMISE | 0.000 | 0.000 | 0.000 |
| binding request bridge / receive / `node-raw-binding-port.js` | PROMISE-callback | 0.000 | 0.000 | 0.000 |
| binding request bridge / receive / `node-raw-binding-port.js` | sampled heap B | 909.2 | 758.9 | 764.0 |
| binding future / admission / `completion_owner.js` | PROMISE | 0.000 | 0.000 | 0.000 |
| binding future / admission / `completion_owner.js` | PROMISE-callback | 0.000 | 0.000 | 0.000 |
| binding future / admission / `completion_owner.js` | sampled heap B | 83.4 | 87.7 | 86.3 |
| raw protocol / ingress (both) / `raw-service-mesh-runtime.js` | PROMISE | 9.430 | 0.086 | 0.086 |
| raw protocol / ingress (both) / `raw-service-mesh-runtime.js` | PROMISE-callback | 6.016 | 0.000 | 0.000 |
| raw protocol / ingress (both) / `raw-service-mesh-runtime.js` | sampled heap B | 3407.5 | 871.6 | 773.8 |
| backend / completion / poll (both) / `node-raw-mesh-backend.js` | PROMISE | 3.086 | 0.211 | 0.211 |
| backend / completion / poll (both) / `node-raw-mesh-backend.js` | PROMISE-callback | 1.547 | 0.109 | 0.109 |
| backend / completion / poll (both) / `node-raw-mesh-backend.js` | sampled heap B | 1595.3 | 948.6 | 944.8 |
| worker / terminal (both) / `mesh-dispatch-pump.js` | PROMISE | 7.930 | 2.258 | 2.258 |
| worker / terminal (both) / `mesh-dispatch-pump.js` | PROMISE-callback | 4.570 | 2.117 | 2.125 |
| worker / terminal (both) / `mesh-dispatch-pump.js` | sampled heap B | 2231.1 | 304.3 | 321.9 |
| target handler / reply / `channel-dispatch-pipeline.js` | PROMISE | 4.000 | 2.000 | 2.000 |
| target handler / reply / `channel-dispatch-pipeline.js` | PROMISE-callback | 2.000 | 1.000 | 1.000 |
| target handler / reply / `channel-dispatch-pipeline.js` | sampled heap B | 1511.9 | 816.4 | 772.5 |
| target activation / `channel-dispatch-services.js` | PROMISE | 5.000 | 1.000 | 1.000 |
| target activation / `channel-dispatch-services.js` | PROMISE-callback | 2.000 | 0.000 | 0.000 |
| target activation / `channel-dispatch-services.js` | sampled heap B | 824.6 | 45.1 | 48.1 |
| target host claim cleanup / `admission.js` | PROMISE | 2.000 | 1.000 | 1.000 |
| target host claim cleanup / `admission.js` | PROMISE-callback | 1.000 | 1.000 | 1.000 |
| target host claim cleanup / `admission.js` | sampled heap B | 0.0 | 38.3 | 35.6 |
| target permit cleanup / `application-job-queue-scope.js` | PROMISE | 3.000 | 1.000 | 1.000 |
| target permit cleanup / `application-job-queue-scope.js` | PROMISE-callback | 2.000 | 1.000 | 1.000 |
| target permit cleanup / `application-job-queue-scope.js` | sampled heap B | 629.4 | 157.1 | 162.5 |
| target node dispatch / `spot-node-runtime-manager.js` | PROMISE | 2.000 | 2.000 | 2.000 |
| target node dispatch / `spot-node-runtime-manager.js` | PROMISE-callback | 1.000 | 1.000 | 1.000 |
| target node dispatch / `spot-node-runtime-manager.js` | sampled heap B | 246.7 | 494.4 | 496.7 |
| source/native / `pollerWaitInto` | native calls | 0.008 | 0.008 | 0.008 |
| source/native / `routerRecvMessageNoWait` | native calls | 0.008 | 0.008 | 0.008 |
| source/native / `socketSubmitSend` | native calls | 1.000 | 1.000 | 1.000 |
| target/native / `pollEventsFd` | native calls | 0.008 | 0.008 | 0.008 |
| target/native / `pollEventsRevents` | native calls | 0.008 | 0.008 | 0.008 |
| target/native / `pollEventsSlot` | native calls | 0.008 | 0.008 | 0.008 |
| target/native / `pollEventsSourceKind` | native calls | 0.008 | 0.008 | 0.008 |
| target/native / `pollerWaitInto` | native calls | 0.008 | 0.008 | 0.008 |
| target/native / `routerRecvMessageNoWait` | native calls | 1.008 | 1.008 | 1.008 |
| shared/native / `messageFrameClose` | native calls | 2.000 | 2.000 | 2.000 |
| shared/native / `messageFrameCopyData` | native calls | 2.000 | 2.000 | 2.000 |

source completion은 마지막 커밋에서 hash 계산 bucket 4회/건이 생겼지만, 이번 수정의 목적이 된 64배 수준 queue 판정 반복과 다릅니다. 실제 원수치와 전체 module 계수는 `logs/*-coefficients.log`, heap 원수치는 `logs/*-heap.log`, runner는 `coefficients.cjs`에 보존했습니다. send의 Promise 감소는 최종에서도 유지됩니다. 이전 보고의 47.6→9.2는 독립 target process 측정이고 이번 표는 두 host 합계이므로 두 수치를 직접 같은 분모로 비교하지 않습니다.

## 검증

- 수정 전 regression: 실제 receive-round 경로에서 `pressure evaluations per request: 65 > 4`로 실패(`red-rotation.log`). 동일 regression을 parent에서 실행하면 통과하며 diagnostic 4입니다(`parent-rotation.log`).
- 수정 후 관련 34/34 통과(`green-rotation.log`): 단일 회전 cost, FIFO waiter, 활성 storage 유지, 기존 application queue·node path budget.
- main / 수정 전 rebase snapshot / final 전체 runtime 독립 검사와 m6a·m6b·m6c 및 format 결과는 아래 표입니다. 표의 final 파일명 `rebase-*`는 처음 runner의 variant 이름을 유지한 것입니다.
- 기본 `npm test`는 빌드 단계에서 Unity 메타 누락으로 멈춥니다. 이 실패는 `main-unit.log`, `archived-rebase-unit.log`, `rebase-unit.log`에 보존했습니다. 표의 runtime 독립 실행은 gate 사본에서 build 호출만 분리하고 원래의 typecheck·lint·전수 test enumeration·TAP integrity 검사를 실행했습니다. 전체 gate 통과로 부르지 않습니다.
- source 전체 build와 browser codec은 로컬 실행했습니다. typecheck/lint는 독립 runtime runner에 포함됩니다. `scripts/format/format.sh --check node` 및 추가 변경 JS test Prettier 검사 통과입니다.
- 처음 복사된 binding 1.2.0과 dist 없는 HTTP archive로 얻은 초기 실패는 `invalid-http-*`로 분리했으며 baseline 실패로 계산하지 않습니다. native 1.19.0 manifest와 local HTTP client 0.30.0 build/archive를 준비한 결과만 비교했습니다.

| ref | runtime announced/completed | 실패 assertion | m6a | m6b | m6c | Node format |
|---|---|---|---|---|---|---|
| main | 2618/2618 | 동일 Unity 2개 | 44/44 | 132/132 | 250/250 | 통과 |
| rebase snapshot | 2654/2654 | 동일 Unity 2개 | 44/44 | 132/132 | 250/250 | 통과 |
| final | 2656/2656 | 동일 Unity 2개 | 44/44 | 132/132 | 250/250 | 통과 |

BLOCKERS: `framework/languages/unity/com.zlink.stream-connector.webgl/LICENSE.meta`는 있지만 `LICENSE` 파일은 없고 `LICENSE.md`로 바뀌어 있습니다. Node `scripts/sync-unity-webgl-package.mjs:65–76`은 `LICENSE.md.meta` 누락으로 빌드를 거부합니다. `test/contract/unity-webgl-package.test.js:40`(LICENSE 없음), 동일 파일 두 번째 test의 bundle 동기화도 실패합니다. main·rebase·final 공통이며 Unity는 승인된 언어 범위 밖이므로 이 부분은 수정하지 않았습니다. 스펙 gap BLOCKERS는 없습니다.

## 판정 위치와 마무리 점검

| 한정한 결정 | 전 | 후 |
|---|---:|---:|
| permit terminal phase 전환 | 1 | 1 |
| reserved/queued capacity underflow 판정 | 2 | 1 |
| FIFO waiter grant | 1 | 1 |
| pressure 상태 판정 | 1 | 1 |
| 합계 | 5 | 4 |

위 수는 runtime 전체 분기 수가 아니라 변경한 반환 결정을 내리는 위치 수입니다. 미사용 회전 처리의 건별 반복 평가를 일괄 반환으로 줄였으며, phase eligibility·재사용 storage 판단은 기존 위치를 사용합니다. 추가/삭제된 상태·flag·queue·lock·timer·retry는 모두 0입니다.

변경 파일: `framework/languages/node/packages/framework/src/runtime/host/application-job-queue.ts`, `framework/languages/node/test/contract/node-request-rotation-cost.test.js`. 다른 언어·spec 파일은 수정하지 않았습니다.


커밋 SHA: dfec2a09af6c410afef2c71469f0e2103f22c4ae. Refs #1675, Refs #1639. push·PR·merge·reset·stash·브랜치 전환 없음. Project #1675는 Review로 갱신했습니다. #1639 epic 상태는 다른 언어 작업이 진행 중이므로 유지합니다.

WSL 검증 사본: `/home/hep7/worktree/zlink-1675-node`. 로그·스크립트·원수치를 `D:/t/jobs/fx/cx/m1639n3/`에 보존한 뒤 사본을 제거했습니다. 초기 PowerShell 경로 인용으로 worktree에 생긴 본 job의 npm cache는 `initial-npm-cache/`로 옮겼습니다. 이후 모든 WSL 명령은 사본 안의 script에서 TMPDIR/npm cache를 설정했습니다.

리팩토링 점검: 성능 1건·POSDDD 1건·불필요 코드 1건 발견, 미사용 예약의 반복 drain/pressure 판정을 일괄 반환으로 고치고 기존 queue owner와 terminal 전환을 재사용했으며 reserved/queued underflow 검사 사본을 통합했습니다. 넘긴 항목: Unity LICENSE/메타 불일치(`framework/languages/node/scripts/sync-unity-webgl-package.mjs:65`, `framework/languages/unity/com.zlink.stream-connector.webgl/LICENSE.meta:1`, 수정 범위 밖).

