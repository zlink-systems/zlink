현재 결과는 실행 검증 전 초안이다. Core gate 파일 `/root/jobs/fx/cx/v21/s033/core124b/verdict.md`가 없어 빌드, 테스트, lock 획득과 CPU 측정을 실행하지 않았다. 아래 변경은 컴파일과 동작 확인이 남아 있다. 성능 개선이나 테스트 통과를 주장하지 않는다.

1. 변경

경로의 `C`는 worktree의 `framework/languages/cpp/framework/src/runtime/`이고, `T`는 `framework/languages/cpp/tests/`이다.

`C/backend/raw_route_port.cpp:473`은 호출자가 제공한 public binding `received_t`에 바로 수신한다. `C/mesh/service_mailbox.hpp:150`의 canonical record가 그 저장소를 소유한다. parts vector를 별도로 소유하던 `raw_received_t`는 binding 타입의 alias로 바뀌었다. 기존 내부 생산자와 테스트는 parts accessor와 `from_parts`를 사용한다. record의 aggregate와 move-only 성질은 유지한다.

`C/backend/raw_route_port.cpp:532`의 reply는 native typed RID와 원래 reply token을 사용한다. `C/stateful/public_host_runtime.cpp:8365`도 record의 typed RID를 전달한다. 일반 ingress는 기존 span 기반 topology lookup을 사용하고, logical source identity는 admitted descriptor에서 복사한다. native RID를 byte vector로 바꿨다가 다시 만드는 과정은 일반 수신·응답 경로에서 없어진다. 아직 admit되지 않은 handshake의 wire 계약에 필요한 byte identity와 local record의 byte identity는 유지한다.

`C/mesh/raw_mesh_node_owner.cpp:3362`는 회전 시작 시 receive port lease를 확보한다. `:3594`의 record 수신은 그 회전이 보유한 lease를 사용한다. `C/backend/raw_route_port.cpp:553`의 socket/poller close gate는 유지한다.

`C/mesh/service_mailbox.cpp:25`는 첫 service prefix 해석 결과와 application kind를 record에 보관한다. ingress가 정한 owner와 이 결과가 mailbox를 통과한다. `C/stateful/public_host_runtime.cpp:8139,8353`은 이를 재사용한다. command별 최초 shape, bounds, native route generation과 logical peer 검사는 유지한다. stateful ingress도 기존 claim 구조 안에서 같은 검증 결과를 전달한다.

`C/mesh/raw_mesh_node_owner.cpp:3651,4229`의 trace 문자열은 enabled 분기 안에서 만든다. protocol error 결과와 기존 거부 reply는 유지한다. mailbox claim/settlement, dispatcher 객체, scope, coroutine 구조, worker 수, wake 정책과 W5는 바꾸지 않았다.

2. 결정 소유와 개수

이 변경은 승인된 설계 §7의 1행이며 분류는 B다. public 동작을 바꾸지 않고 binding 소유 저장소와 routing capability를 재사용한다.

| 결정 | 소유 계층과 spec 근거 | 기존 판정·표현 위치 | 변경 뒤 소유자 |
|---|---|---|---|
| native parts, routing id, reply token, route generation의 수명 | binding. S05 §2·4와 설계 §2·3 | raw route wrapper, mailbox parts/token, dispatch RID 재구성 | canonical record의 public `received_t` |
| native reply 라우팅과 token 유효성 | binding. 설계 §3, wire §2의 원래 request reply 경로 | `raw_route_port.cpp:524`의 RID 재구성 | binding의 typed RID/token reply builder |
| service kind와 owner의 ingress 분류 | Framework ingress. S08 §4.1 I3, wire §2·4 | `raw_mesh_node_owner.cpp:3647`, `public_host_runtime.cpp:8165` 및 worker의 prefix 재해석 | record의 첫 service 분류와 ingress owner |
| receive port의 C++ wrapper 수명 | Framework port lifecycle. 설계 §3·§8 | 회전 시작과 record마다 lifecycle mutex/port lookup | 회전이 보유한 lease |
| socket/poller와 close의 동시 접근 | Framework port gate. 설계 §3의 wrapper close 규칙 | port socket mutex와 poller mutex | 같은 gate 유지 |
| application FIFO, claim, settlement와 실행 권한 | Framework mailbox와 execution gate. S02, S04 | 기존 mailbox와 host claim 경로 | 이번 단계에서 유지 |

.NET은 `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:718`에서 typed record kind와 payload owner를 전달한다. Java는 `runtime/binding/ZLinkJavaRawMeshNode.java:5979`에서 읽은 header를 record에 보관하고 `:6030`에서 다시 사용한다. Kotlin은 `zlink-framework-kotlin/build.gradle.kts:13`이 내보내는 Java core runtime을 공유한다. 이 대조에서는 사용자 동작을 다르게 정해야 하는 spec gap을 찾지 않았다. 이번 작업은 C++ 내부 표현과 소유권 전달만 바꾼다.

| 계수 범위 | 수정 전 | 수정 후 코드의 의도 | 실행 확인 |
|---|---:|---:|---|
| 설계가 지정한 service-kind 판정 위치 | 2 | 1 | 미실행 |
| 일반 send의 native source RID 변환 | 2 | 0 | 미실행 |
| 일반 request/dispatch/reply의 native source RID 변환. 기존 reject context 사전 변환 포함 | 4 | 0 | 미실행 |
| K건을 읽는 회전의 begin + record별 receive lease | 1+K | 1 | 미실행 |
| 회전 안의 record별 receive lease | K | 0 | 미실행 |
| 일반 receive body deep copy | 0 | 0 | 미실행 |
| native reply token authority | 1 | 1 | 미실행 |
| socket/poller close gate | 1 | 1 | 미실행 |

RID 변환 수는 수신한 native source RID에 한정한다. application fence나 logical descriptor의 byte identity까지 없앴다는 뜻이 아니다. idle wait, 관리와 송신의 별도 lifecycle 접근도 receive record lease 개수에 합쳐 숨기지 않는다.

3. 테스트

`T/Zlink.Framework.UnitTests/test_cpp_framework_receive_record.cpp`에 테스트 초안을 추가했다. 기존 native-call counter와 body materialization 관찰을 함께 사용한다. 한 회전의 세 request에서 native body copy 0, body materialization 0, service classification 3, port lease 1을 확인하고, 각 record의 pointer, typed RID, route generation과 token identity를 ingress 원본과 비교한다. 실제 reply completion도 확인한다.

| 항목 | 정상·음성 대조 | 현재 결과와 log |
|---|---|---|
| native body copy | `ZLINK_RECEIVE_RECORD_NEGATIVE=copy`로 `.copy()` 한 번 추가 | 미실행 |
| body materialization | `clone`으로 native clone 한 번 추가 | 미실행 |
| service 분류 | `classification`으로 첫 record를 한 번 더 분류 | 미실행 |
| receive lease | `lease`로 같은 회전에서 실제 lease 한 번 추가 | 미실행 |
| 원래 reply token | `token`으로 다른 request의 token과 비교 | 미실행 |
| caller wrapper 파괴와 queued record 수명 | caller wrapper 파괴 뒤 원래 storage와 token 사용. `drop`은 조기 release 대조 | 미실행 |
| 회전 중 close와 close 뒤 reply | poll/close 동시 실행, 보유 lease로 후속 receive, reply 거부 | 미실행 |
| malformed prefix, truncated header, 잘못된 parts 수 | protocol error와 복구 가능한 request의 기존 wire terminal. 정상 shape 수락과 `malformed` 대조 | 미실행 |
| 전체 C++ Framework unit·contract, non-install | CTest entry 수와 pass/fail을 기록할 예정 | 미실행 |
| receive/route/mailbox 20회 부하 반복 | `stress-ng --cpu 16 --cpu-load 80`, `measure-shared.sh` 사용 | 미실행 |
| 필수 C++ format check | `bash scripts/format/format.sh --check cpp` | 미실행. 수정 파일의 format 쓰기만 수행 |

실행용 스크립트는 `scripts/build-tests.sh`, `scripts/stress-tests.py`에 준비했다. 성공·실패·음성 대조 log와 XML을 모두 `raw/` 아래 보존해야 한다. 음성 대조는 해당 정상 assertion이 실패하는 출력까지 확인해야 완료다.

중단 뒤 남은 실행 프로세스와 부분 측정 파일은 없었다. `raw/start-status.txt`, `raw/resume-status.txt`에 현재 실행 경계를 기록했다. `.stopped`로 옮길 측정 출력은 없었다.

4. 같은 rate의 target CPU

main과 change의 paced 3회 측정은 모두 미실행이다. 따라서 target user+kernel CPU µs/message의 차이와 설계의 0.2–0.5 µs/message 추정은 아직 비교할 수 없다.

허용된 `csrc/scripts/measure.py`를 `scripts/measure.py`로 복사해 paced main/change 세 회만 실행하도록 조정했다. rate는 원본과 같은 136,889 message/s다. source는 CPU 0–7, target은 8–15를 사용하고, MemAvailable 12 GiB 이상을 확인한다. 측정은 `flock -o /root/jobs/fx/cx/measure.lock` 안에서만 실행해야 한다. target CPU는 기존 active 구간의 `server_cpu_percent × logical_cores / 100`을 그 구간의 message rate로 나눠 user+kernel µs/message로 계산한다.

현재 worktree의 bench client에는 `CSEND_PACE` 처리가 없으므로 `scripts/prepare-measure.py`는 측정용 source 두 사본에 동일한 pacing을 넣는다. 이 코드는 제품 diff에 넣지 않는다. `scripts/build-measure.sh`는 Release preset, `-O3 -DNDEBUG`, debug flag 없이 두 variant를 빌드하도록 준비했다. throughput cell이나 unpaced run은 실행하지 않는다.

5. 리팩토링 점검

| 수신 경로의 중복 검사·handoff·copy·wrapper | 위치 | 이번 단계 |
|---|---|---|
| parts vector를 소유하는 중간 raw wrapper | 기존 `raw_route_port.cpp:473` | binding `received_t` 저장소로 통합 |
| native RID → byte vector → typed RID | 기존 `raw_route_port.cpp:495,524`, 기존 host RID 구성 | 일반 native 수신·dispatch·reply에서 제거 |
| record별 lifecycle mutex와 port 조회 | 기존 `raw_mesh_node_owner.cpp:3609` | 회전 시작 한 곳으로 이동 |
| ingress와 host의 service kind 재판정 | 기존 `raw_mesh_node_owner.cpp:3647`, `public_host_runtime.cpp:8165` | record의 첫 분류 결과로 통합 |
| disabled trace의 문자열 구성 | 기존 `raw_mesh_node_owner.cpp:3649,4242` | enabled 분기 안으로 이동 |
| receive → mailbox commit | `service_mailbox.cpp`의 기존 commit | 유지. 새 queue나 handoff를 추가하지 않음 |
| claim prefix → host batch → record dispatch | `service_mailbox.cpp`, `public_host_runtime.cpp` | 유지. 설계의 다음 단계 범위 |
| stateful ingress의 native header/body view retain | `raw_stateful_dispatch.cpp`의 `claimed_ingress_t::prepare` | 유지. body deep copy와 구별 |
| per-record mesh/route dispatcher와 reply closure | `mesh_record_dispatcher.cpp` | 유지. 설계의 다음 단계 범위 |
| command별 fence와 shape 검증 | ingress 및 stateful authority 경계 | 유지. service owner 재분류와 구별 |
| socket/poller gate와 binding call guard | `raw_route_port.cpp:473,553` 및 binding | 서로 다른 수명 경계이므로 유지 |

수정 전/후 판정 위치 수는 service-kind 2→1, record별 receive port lookup 1→0과 회전 lease 1→1이다. native storage 원본 외에 queue, timer, completion flag나 lock을 추가하지 않았다. 검증 결과를 record에 보관하는 필드와 회전의 port 참조를 추가하면서 별도 raw parts/token 저장소와 반복 분류·lookup을 없앴다. dispatcher, claim, scope와 wake 관련 후속 단계는 이번 변경에 포함하지 않았다.
