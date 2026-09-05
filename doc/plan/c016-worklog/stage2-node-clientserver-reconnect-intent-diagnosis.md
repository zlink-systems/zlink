# Node ClientServer reconnect intent 진단

감독이 Node ClientServer의 transport 복구 중복과 Stage 2 수정 범위를 판단하기 위한 기록이다.
이 문서는 runtime 수정 전에 작성했다. 작업 요청의 Stage 1+2 승인은 아래 A/B 변경에 적용한다.

## 계약과 선행 구현

- 소유 계층: Core는 connect intent, command progress와 physical pipe 교체를 소유한다. Framework는 service handshake, descriptor 검증, logical liveness, endpoint 종료 요청과 close 관찰을 소유한다.
- Spec 조항: Framework `02-channel-transport/05-transport-liveness.ko.md` §5·§6 (`3a2ada8187`); Core `core/doc/spec/core/socket/README.ko.md` §4 RID 중복 정책·§6 `zlink_connect`/`zlink_disconnect`, `05-polling.ko.md` §3, `06-monitoring.ko.md` §3.1·§3.2.
- 교차언어 대조: .NET `05b53a8098`의 `ZLinkClientServerClientRuntime.cs:990-1014`는 일반 disconnect에서 기존 intent를 유지하며, 명시적 endpoint 종료 때만 해당 endpoint의 close를 관찰한 뒤 `Connect`를 한 번 호출한다. `:1472-1501`은 logical admission fence와 종료 요청을 소유한다. 두 번째 poller와 reconnect/admission retry loop는 없다. C++의 기존 intent 유지와 같은 소유권이며 Node의 재생성은 언어의 구조적 요구가 아니다.
- 변경 분류: **A — 하위 계층 계약 적응**, malformed pushed control의 endpoint 종료 누락은 **B — 기존 결함**.

`0c39ed2e52`는 application poll 없는 disconnect terminal과 재연결 READY 진행을,
`7cbf12de41`은 completion poller 등록 뒤에도 monitor의 command lease 유지를 보장한다.
이 보장은 명시적 `disconnect(endpoint)`가 삭제한 intent를 자동 복원한다는 뜻이 아니다.
Core README §6 `:857-866`은 기존 연결 제거를 규정하고, .NET commit도 close 뒤 1회
intent 복원을 남긴다. 요청의 “재등록 제거”를 모든 `Connect` 호출 제거로 해석하면 이 계약과
충돌한다. 감독에게 이 차이를 전달했으며, 구현 기준은 지정된 .NET counterpart와 같은
**일반 장애 시 0회, 명시적 종료의 close 관찰 뒤 1회, timer/실패 반복 0회**다.

## Client path inventory

위치는 수정 전 source 기준이다. `registry`는
`framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts`,
`location`은 같은 디렉터리의 `client-server-location-runtime.ts`다.

| 위치 | 현재 결정 | 소유자와 수정 |
|---|---|---|
| registry `:346-451`, `:671-701` | 설정·descriptor마다 DEALER, monitor, receive poller와 최초 connect를 만든다. | Framework의 configuration intent 등록. 유지. |
| registry `:408-437` | READY에서 admission을 시작하고 disconnect/close/handshake failure에서 admission·ready를 해제한다. | Framework §5·§6. 관찰 뒤 handshake 재실행 유지. native connection_id를 fence로 사용하지 않는다. |
| registry `:865-888` | liveness deadline에서 callback을 먼저 종료 통지하고, `reconnectOnTermination`이면 즉시 disconnect/connect한다. | deadline은 Framework, 반복 물리 복구는 Core. 종료 요청과 해당 endpoint close 관찰로 통합하며 즉시 connect와 callback별 reconnect 옵션 제거. |
| registry `:1184-1201` | manual/local READY callback에서 admission을 시작하고 실패 시 ready 해제·오류 통지한다. | Framework. 자체 retry 없음. 이전 attempt의 실패가 새 admission을 해제하지 않도록 기존 attempt fence로 처리한다. |
| registry `:1204-1249` | physical observation/attempt token을 확인한 뒤 handshake 결과를 admit한다. | Framework의 logical admission fence. 유지. |
| registry `:1253-1295`, `:1303-1348` | malformed pushed control, 금지 command, immutable identity·bound·revision 오류에서 ready만 해제한다. | Framework §5 종료 요청 누락(B). liveness와 같은 endpoint 종료 경로를 사용하고 close를 기다린다. |
| registry `:1386-1425` | liveness probe를 같은 probe ID로 반복하고 ACK/deadline을 확인한다. | Framework의 liveness 프로토콜. Core connect retry가 아니므로 유지. |
| registry `:644-659` | outbound가 기존 admission 완료를 제한 시간 동안 관찰한다. | Framework의 호출 시 ready 대기. connect/admission을 재제출하지 않으므로 유지. |
| location `:194-236` | 현재 descriptor가 없으면 새 target을 만들고 descriptor 교체·제거를 reconcile한다. | Framework의 discovery 구성 변경. 유지. unchanged descriptor의 transport 실패로 target을 삭제하지 않는다. |
| location `:301-305`, `:323-374` | admission 실패마다 close하여 다음 Store poll에서 새 DEALER/admission을 시도한다. | transport 실패의 물리 재시도는 Core 소유. 실패를 보고하고 반복 생성 제거. identity/configuration 거부의 target retirement는 유지하며 unchanged descriptor poll로 재시도하지 않는다. |
| location `:308-309`, `:390-394` | transport 종료마다 target 삭제·DEALER dispose 후 다음 Store poll에서 재생성한다. | Core intent와 중복(A). 기존 target/socket을 보존하고 논리 admission만 pending으로 전환한다. 이전 attempt 결과는 target identity로 차단한다. |
| registry `:454-523`, `:539-556`; location `:376-387`, `:397-405` | configuration removal, descriptor replacement와 alias 소유권 정리 시 dispose한다. | Framework의 configuration lifecycle. transport 장애 복구와 구분해 유지. |

## 대안과 규칙 수

1. 즉시 disconnect/connect만 지우고 자동 discovery의 delete/recreate를 유지하면 Core와
   Framework가 같은 장애의 복구를 계속 결정한다. malformed control의 close 누락도 남는다.
2. 기존 socket/intent를 보존하고 종료 요청·관찰을 registry로 모은다. discovery는 descriptor와
   logical handshake 결과만 소유한다. 명시적 종료 뒤 intent 복원은 .NET과 같은 단발 호출이다.

대안 2를 적용한다. reconnect timer, 두 번째 poller, native connection_id map을 추가하지 않는다.
**수정 전/후 규칙 수: 물리 복구 결정 4 → 2** — Core auto reconnect + Framework 즉시 재등록 +
discovery 종료 후 재생성 + admission 실패 후 재생성에서, Core auto reconnect + 명시적 종료의
close 관찰 뒤 intent 1회 복원으로 줄인다. handshake 재실행·liveness deadline은 기존 Framework
규칙으로 유지한다. 신규 종료 관찰 상태는 callback별 `reconnectOnTermination` 정책을 대체한다.

## 검증 범위

Malformed pushed control의 not-ready → endpoint 종료 요청 → 해당 endpoint close 관찰 → 단발
intent 복원 → Core READY → handshake 재승인을 fake monitor의 순서 검증과 native HANDOVER
ROUTER 회귀로 확인한다. 일반 disconnect, 늦은 admission 응답, discovery poll과 admission 실패가
connect/new DEALER 루프를 만들지 않는지도 검증한다. 기존 assertion과 시간 예산은 낮추지 않는다.

지정 환경에서 touched contract → `npm run typecheck` → touched lint → `npm test` 1회 →
SupportChat.Ts·ShoppingMall.Ts 각 1회 순서로 실행한다. `TMPDIR=/dev/shm/zlink-tmp-node`,
`unset ZLINK_LIBRARY_PATH`, `flock -w7200 /tmp/zlink-node-gate.lock`을 적용하고 기존 package를
사용한다. Core/binding 재빌드·수정, 다른 언어·보호 문서 수정과 commit은 하지 않는다.

## BLOCKERS

진단 시점에 확인한 Core/binding 결함은 없다. 모든 명시적 종료 뒤 `Connect`까지 금지하는
해석은 현재 계약으로 구현할 수 없으며, .NET과 동일한 단발 intent 복원과 구분해야 한다.

## READY snapshot 추가 진단

전체 gate에서 native inproc 회귀가 handshake 3회로 실패했다. 기존 monitor의 `value`를
test 진단에 보존해 좁혀 재현한 결과는 `DISCONNECTED → READY(value=0) → READY(value=1)`이다
(`/tmp/zlink-stage2-node-reconnect-intent/inproc-diagnosis-1.log`). Core는 disconnect count
snapshot과 새 연결의 READY를 구분해 전달했고, Node registry `:418-425`가 value 0까지 새
handshake로 처리했다. Core/binding 결함이 아니다.

- 소유 계층: Core가 monitor count를 발행하고 Framework가 단일 endpoint DEALER의 ready 여부를 해석한다.
- Spec 조항: Core `core/doc/spec/core/06-monitoring.ko.md` §3.1·§3.2. READY value는 현재 ready transport 수이며 count snapshot을 새 연결로 판정하지 않는다.
- 교차언어 대조: .NET `ZLinkClientServerClientRuntime.cs:964-969`도 `Value == 0`이면 admission을 시작하지 않는다. Node backend는 이미 bigint value를 손실 없이 전달한다.
- 변경 분류: **B — 기존 결함**. Node에서 value 0 snapshot으로 admission을 시작하지 않도록 고친다. 새 native ID fence·상태·timer·API는 필요 없다. 물리 복구 결정 수 **4 → 2**는 그대로다.

대안은 backend에 flags API를 확장하는 것과, 기존 value를 .NET처럼 해석하는 것이다. 단일
endpoint DEALER의 disconnect snapshot에는 기존 value로 충분하므로 후자를 적용한다.
