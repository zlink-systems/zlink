[English](./framework-cpp-0.32.0.md) | [한국어](./framework-cpp-0.32.0.ko.md)

# ZLink C++ Framework 0.32.0 릴리스 노트

Framework 0.32.0은 C++ binding 1.23.0과 Core 1.23.0을 사용합니다. (#3843)

## 변경 사항

- C++ server와 Stream Connector가 control frame 규칙을 따릅니다. 알 수 없는 control packet이나 heartbeat payload version을 받아도 연결을 닫지 않습니다. (#3773, #3820)
- C++는 relay된 Actor message에서 중복되거나 더 낮은 session sequence를 거부합니다. (#1721, #3783)
- `MessageContext.receivedAt`은 Framework가 message를 받은 UTC 시각입니다. 기본값이 없는 DTO property는 framework-json-v1에서 필수입니다. (#2029, #3602, #1705, #3592)
- 중복 envelope header field를 거부합니다. 중복 bind는 owner Actor의 `onDuplicateBind` callback이 판정합니다. (#3376, #3396, #2205, #3640)

## 결함 수정

- Spot 생성, Store 조회, mesh·fanout 게시와 Actor authority 갱신이 lane 작업을 반환하므로 infrastructure worker가 lane 완료를 기다리지 않습니다. Auto-connect 갱신도 worker 완료를 기다리지 않고 진행합니다. (#2960, #3748, #3743, #3663, #3758, #3770, #3841, #3861)
- Fanout descriptor 취소가 domain stop source를 사용합니다. Host 종료는 owner와 transport를 정리하기 전에 Close gate를 거쳐 Entry Spot을 닫습니다. (#3850, #3856, #3776, #3786)
- role이 None인 MeshNode는 in-process Location Store 없이 실행됩니다. (#3721, #3752)
- capturing lambda 안에서 coroutine을 실행하지 않습니다. 입력과 request lifecycle 상태는 coroutine frame에 남습니다. (#3691, #3775, #3594, #3696)
- 기존 Spot·node request는 deadline 하나를 유지하며, resolver 대기는 deadline 또는 shutdown 때 끝납니다. (#3580, #3660)
- Stream Connector의 connect와 cancel socket 작업을 같은 strand에서 실행합니다. (#3534, #3545)
- 늦게 도착한 physical 결과는 fence가 일치할 때만 route cache 항목을 무효화하며, cache 무효화는 08-routing §2의 trigger 표를 따릅니다. 강제 종료 뒤 Store 정리는 deadline 하나를 쓰고 authority를 기준으로 sweep하며, 대상 lifecycle이 끝나면 reservation을 해제합니다. (#3571, #3771, #2989, #3409, #2646, #3462, #3583, #3697)
- relocation terminal 결과를 message flow에 기록하고 cutover flow와 Bingo join·observer 정책을 보존합니다. (#2465, #3621)
- callback 실패에서 원래 exception을 보존합니다. correlation을 읽을 수 있는 잘못된 request에는 terminal protocol error를 응답합니다. (#2098, #3429, #2512, #3418)
- native completion부터 typed reply까지 request result의 소유자를 하나로 유지합니다. decode 실패는 원래 exception을, authority 조회 실패는 기존 오류 분류를 보존합니다. native Spot target 준비 실패는 해당 claim record만 정산합니다. (#3600, #3472, #3507, #3358, #3384, #3394, #3403)
- Actor gateway storage의 shared_ptr 순환 소유를 제거합니다. (#3506, #3520)
- Instance-intent request는 admission 전에 stale route를 거부하고 request deadline 안에서 현재 authority를 따릅니다. disconnect가 실패해도 liveness 정리가 peer 정리와 통지를 마칩니다. (#3353, #3442, #2370, #3434)
- Auto-connect는 같은 endpoint의 descriptor만 자기 자신으로 제외하며, 게시가 끝난 뒤 peer를 표시합니다. binding 완료 뒤 도착한 terminal reply는 Actor slot을 해제하고 Session reply로 보냅니다. (#2155, #3433, #3569, #3664, #2757, #3419)

## 성능 개선

- receive와 Spot reply 경로에서 payload 복사를 없앴습니다. 준비된 send 결과는 inline으로 반환하고 canonical envelope header는 fast path를 사용합니다. (#3693, #3768, #1672, #3518, #3375)
- 직접 처리하는 Node·Channel record는 Location owner-lease 조회를 건너뜁니다. message flow가 꺼져 있으면 trace 작업도 건너뜁니다. (#3607, #3568, #3623)
- worker pool 크기를 process에서 사용할 수 있는 CPU 수에 맞춥니다. (#3555, #3557)

## 시험과 CI

- regex source scan을 target_contract 밖으로 옮기고 capturing-coroutine scanner를 최적화된 gate로 실행합니다. (#3826, #3831, #3838)

## 설치

[GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.32.0)에서 플랫폼별 Framework archive를 선택합니다. Server Framework는 C++ binding 1.23.0과 Core 1.23.0을 사용합니다. Stream Connector만 사용하는 client에는 Core와 binding이 필요하지 않습니다.

릴리스 태그는 [`framework-cpp/v0.32.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.32.0)입니다.
