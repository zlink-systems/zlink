# Node 수신 시각 중복 제거

## 수신 시각 생성과 전달

`framework/languages/node/packages/framework/src/runtime/foundation/service-mailbox.ts:112`에서 mailbox가 처음 enqueue할 때 `receivedAtClock()`으로 시각을 기록한다. mailbox는 같은 record 객체를 유지하므로 queue 대기 중 이 값은 바뀌지 않는다(`service-mailbox.ts:363-368`). 일반 application record는 `node-raw-mesh-backend.ts:2674`에서 `ReceiveRecord.receivedAt`으로 전달한다. 한 part짜리 Node send/request는 `:2617`에서, stateful record는 `:2730`에서 `receiveIngressLifecycle`을 사용하고 그 helper는 `:2832`에서 같은 시각을 전달한다.

RouteMesh send는 `channel-dispatchers.ts:818`에서 record 시각을 사용하고, 값이 없을 때만 clock fallback을 호출한다. `dispatchMeshCommand`와 `createRouteContext`가 그 시각을 context에 넣는 위치는 `:895`, `:912`, `:939`다. Request는 `:838`에서 record 시각을 `createRouteContext`로 넘긴다. 값이 없으면 `:929`의 기본 clock이 동작한다. Handler context는 `channel-dispatch-pipeline.ts:412`에서 `receivedAt`을 보존한다. Request handler의 reply 제출은 `node-raw-mesh-backend.ts:2682-2685`의 `record.reply`로 진행되며 새 수신 시각을 만들지 않는다. 이 시각은 reply 자체가 아니라 요청 handler context의 수신 시각이다.

일반 mailbox send/request 경로의 fallback은 mailbox timestamp가 없는 직접 또는 합성 `ReceiveRecord`가 `dispatchMesh`에 들어오는 경우에만 필요하다. 별도 Actor relay 어댑터에는 `host/index.ts:3611`의 누락 시각 fallback도 남아 있다. 일반 mailbox record는 이제 이 fallback에 도달하지 않는다.

다른 Node 수신 경로도 확인했다. ClientServer channel receive loop는 `channel-receive-loops.ts:337,571,769`에서 수신 시각을 만들고, local submit 경로는 `:415,649,867`에서 만든다. 각 경로는 dispatcher로 전달된다(`:364,608,657,822,877`). C++ route record는 `route_packet.hpp:21`의 시각을 `mesh_record_dispatcher.cpp:70`에서 전달한다. .NET은 `ZLinkChannelReceiveLoop.cs:80,149`에서 capture 후 전달한다. Java dispatch value는 `ZLinkChannelDispatchValues.java:107-113,147-148`에서 보관하고 읽는다. Node만 별도 수신 정책을 추가하지 않았다.

스펙 인용은 `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:78-81`이다. “`receivedAt`은 Framework가 message 제출을 수락한 UTC wall-clock 시각이다. 원격 message에서는 Framework가 packet을 받아들인 직후, Actor·Spot serial executor에 넣기 전에 기록한다. 로컬 제출에서는 Framework가 제출을 수락한 시각을 기록한다. 이 값은 monotonic clock이 아니며, 이후 executor queue에서 기다린 시간에 따라 바뀌지 않는다.”

## 변경과 중복 점검

수정 전 일반 mailbox message의 시각 결정 위치는 mailbox `service-mailbox.ts:112`와 dispatcher fallback `channel-dispatchers.ts:818` 두 곳이었다. 수정 후 일반 mailbox message는 mailbox 한 곳에서 결정한다. Dispatcher fallback은 시각이 없는 직접·합성 record용으로만 남겼다. 일반 record 변환은 `node-raw-mesh-backend.ts:2674`에서 `receivedAt: record.receivedAt`을 전달한다. 기존 `receiveIngressLifecycle`은 bare/stateful 경로의 수명 필드까지 만들기 때문에 시각 전달만을 위해 재사용하지 않았다.

점검한 중복 작업은 일반 mailbox record의 수신 시각 재생성 한 건이며 이번 수정으로 제거했다. bare/stateful record의 기존 lifecycle 전달은 그대로 사용한다. 새 queue, lock, cache, flag 또는 state는 추가하지 않았다.

시간의 소유 계층은 Node Framework mailbox ingress이며 스펙 조항은 Message Model §3이다. 교차언어 대조 결과 C++, .NET, Java는 최초 수신 시각을 dispatch까지 전달하고 queue 대기 뒤 다시 만들지 않는다. 변경 분류는 B, 기존 전달 결함이다.

## 테스트와 로그

계약 테스트 `message-context-received-at.test.js`는 send와 request record를 mailbox queue에 10 ms 두고 dequeue한 뒤 handler로 dispatch한다. 두 handler에서 mailbox 시각과 context 시각이 같고 dispatcher clock 호출은 0회임을 확인했다. 전체 Node `npm test`도 통과했다. Runtime gate에서 4,573개 실행 항목이 완료됐고 M6A contract 48개가 통과했다.

Focused test 로그는 `/root/jobs/fx/cx/v21/s033/nrxtime/focused-test.log`이다. 전체 Node test 로그는 `/root/jobs/fx/cx/v21/s033/nrxtime/npm-test.log`이다. 초기 worktree에 Node dependency가 없고 HTTP client tarball도 stage되어 있지 않아, package version 0.32.0을 staging한 뒤 dependency를 설치했다. 설치 로그는 `npm-ci.log`, build 로그는 `build.log`, `bench-build-main.log`, `build-change.log`에 있다. 두 측정 실행 로그는 `measure-main.log`, `measure-change.log`이며 원시 셀 결과는 `main/`과 `change/` 아래에 있다. Node format check와 `git diff --check`는 각각 `format-check.log`, `diff-check.log`에 있으며 둘 다 통과했다.

## RUNS=5 측정

측정은 Node v22.23.3, binding 1.23.0, 1 KiB payload, warmup 10초, active 20초로 했다. `zlink-node` binding과 `zlink-framework-node`를 각 패턴에서 같은 5-run invocation 안에 측정했다. 각 버전에서 20개 셀 모두 완료 수와 target 수신 수가 일치했고 오류는 없었다. 처리량과 비용은 다섯 run의 throughput 중앙값으로 계산했다. Framework 비용은 `1,000,000 / Framework msg/s`, 예산은 `(1,000,000 / binding msg/s) / 0.6`이다.

| 버전 | 패턴 | Binding msg/s | Framework msg/s | Framework ÷ binding | Framework µs/msg | 예산 µs/msg | 예산 초과 µs/msg |
|---|---|---:|---:|---:|---:|---:|---:|
| main | request-serial | 8,568.6 | 5,365.3 | 0.626 | 186.383 | 194.509 | -8.126 |
| 변경 | request-serial | 9,016.5 | 5,483.1 | 0.608 | 182.379 | 184.846 | -2.468 |
| main | send-saturation | 116,093.1 | 76,839.6 | 0.662 | 13.014 | 14.356 | -1.342 |
| 변경 | send-saturation | 117,287.2 | 76,298.4 | 0.651 | 13.106 | 14.210 | -1.104 |

예산 초과 값은 음수이므로 네 결과 모두 예산 안이다. 이 미세 변경에서 처리량 변화는 반복 간 편차 범위이며, 두 패턴 모두 Framework 처리량 회귀로 나타나지 않았다.
