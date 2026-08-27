# 05-transport-liveness 대장 — R# → 새 위치

> 대상 문서: [`framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md`](../../../../../framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md)
> (`29-transport-liveness.ko.md` + `49-internal-liveness-and-state.ko.md` §1 병합).
> R#와 규칙 요지는 [mapping.ko.md §5.5](mapping.ko.md#55-29-transport-liveness--49-internal-liveness-and-state-1)를 따른다.

**병합 경계**: 이 문서는 `49-internal-liveness-and-state.ko.md`의 §1(liveness 단일 기준·확인
신호 비혼입·authority 비침범)만 가져온다. §2(준비 안 된 대상 처리)·§3(시작 순서·상태 값)·
§4(구독자 backpressure)·§5(계측 비용)는 liveness가 아닌 다른 주제이므로 옮기지 않고
[`../49-internal-liveness-and-state.ko.md`](../../../../../framework/doc/framework/common/spec/server/49-internal-liveness-and-state.ko.md)에
그대로 남아 있다.

## 29-transport-liveness

| R# | 새 위치 | 비고 |
|---|---|---|
| R112 | §1 | 표(연결 방식별 확인 방법)로 그대로 이동 |
| R113 | §1 | Manual/automatic 양쪽 Object Client pair는 probe·deadline 미적용 |
| R114 | §1 | 확인 신호는 public API 아님, handler 미수신 |
| R115 | §1 | Owner 사용 기한·STREAM heartbeat·request timeout·Shutdown과 목적 구분 |
| R116 | §2 | 고정값 표(5초/15초) + builder 미공개 규칙 |
| R117 | §2 | Public raw socket API + service protocol만 사용 |
| R118 | §3 | Ready 정의, RouteMesh·ClientServer 15초 deadline 적용 시점 |
| R119 | §3 | Manual `NotRequired` terminal, `not_required` vs `not_connected` 구분 |
| R120 | §3 | 확인 절차 5단계 |
| R121 | §3 | 입력별 영향 표 5행 |
| R122 | §3 | Probe·ACK는 업무 payload·metadata 미포함, queue·handler 미실행 |
| R123 | §4 | PUB/SUB 비대칭 → `livenessProbe`/`livenessAck` 미사용, publisher별 SUB socket 분리 |
| R124 | §4 | 5초 beacon, topic/payload/frame 수 고정값, application topic 충돌 규칙 |
| R125 | §4 | Publisher별 ready 판정, 15초 무수신 시 not-ready |
| R126 | §4 | Beacon도 fanout 손실 규칙 적용, 포화 상태 not-ready는 오탐 아님 |
| R127 | §4 | 수신 독점 상한 — 모든 다중 connection 수신 경로에 적용, 다음 회전 이동 규칙 |
| R128 | §4 | 3축(건수·byte·경과 시간) 동시 적용, peer 단위 회계, **세 값은 미정**(검증 요구에 "판정 불가" 명시) |
| R129 | §4 | Beacon은 application event 아님(응답·queue·trace·metric 모두 미발생) |
| R130 | §5 | Ready 조건 표 2행 |
| R131 | §5 | Ready target 즉시 제거 조건 7가지 |
| R132 | §5 | Connection replacement admission fence 규칙 |
| R133 | §5 | Orderly close·transport disconnect 즉시 반영 |
| R134 | §5 | Peer 하나의 실패가 host 전체를 `Error`로 안 바꿈 |
| R135 | §6 | Connection 잃은 시점별 처리 표 |
| R136 | §6 | Reconnect 규칙 6가지 |
| R137 | §7 | Owner lease/descriptor는 discovery 근거이지 transport ready 증명 아님 |
| R138 | §7 | `Relocate`·`Shutdown` 뒤 필요한 connection deadline까지 유지, 종료 시 timer·subscription 정리 |
| R139 | §9 | Runtime snapshot 상태 6종, reason 구분, metric label 제한 |
| R140 | §9 | Application job pressure와 route 상태(관측 소절) |

## 49-internal-liveness-and-state §1

| R# | 새 위치 | 비고 |
|---|---|---|
| R141 | §2 | "단일 구조가 소유" 규칙을 굵은 규칙 문장 + 이유 불릿으로(라벨 제거) |
| R142 | §2 | "확인 신호와 업무 message를 섞지 않는다"의 비대칭 이유를 §2 이유 불릿으로 흡수. "확인 신호가 application에 도달하지 않는다"는 문장 자체는 §1이 이미 소유하므로 중복 기재하지 않음(S13) |
| R143 | §1(기존 서술이 이미 동등한 내용을 포함) | "기준값은 같고 방법은 topology마다 다르다"는 29 §1의 표·문단과 중복이라 별도 문장을 추가하지 않음 |
| R144 | §8 | "Liveness 판정은 authority를 안 바꾼다" 4개 책임 분리 항목을 굵은 규칙 문장 + 이유 불릿으로(라벨 제거) |

배치되지 못한 R#: 없음.

## 가져오지 않은 49 절

`49-internal-liveness-and-state.ko.md` §2~§5는 liveness가 아니므로 이번 문서로 옮기지
않았다. 옛 위치와 소유 후보는 다음과 같다(mapping.ko.md §4 S9 근거).

| 절 | 규칙 요지 | 옛 위치 | 소유 후보 문서 |
|---|---|---|---|
| §2 | 준비된 대상이 하나도 없어도 host는 시작하고 `serving`이 되며, 준비 안 된 channel은 그 topology만 저하 상태로 표시 | 49 §2 | 06-observability(24-runtime-monitoring §2.2) |
| §3 | 시작 순서 5단계, 상태 값 7종(닫힌 집합), `serving` 공개 시점 | 49 §3 | 03-spot-actor(13-mesh-node §6), 06-observability(24-runtime-monitoring §2.1) |
| §4 | 상태 구독자·metric 수집기는 실행 권한 비점유, 자리 가득 차면 중간 상태 합치기 | 49 §4 | 06-observability(24-runtime-monitoring "합치기") |
| §5 | 계측(message flow tracing) `off` 상태 비용 zero 규칙 | 49 §5 | 06-observability(26-message-flow-tracing §4.1) |

## 이동 후 갱신할 링크

`29-transport-liveness.ko.md`와 `49-internal-liveness-and-state.ko.md` §1이 옛 경로로
쓰던 링크. 새 문서 안에서는 상대 경로를 이미 갱신했으나, 이동(캠페인 §5) 시점에 이
목록으로 외부 참조·anchor 치환표를 만든다.

| 옛 링크 | 새 문서에서 쓴 링크 |
|---|---|
| `01-glossary.ko.md#routemesh` | `../01-glossary.ko.md#routemesh` |
| `01-glossary.ko.md#channelname` | `../01-glossary.ko.md#channelname` |
| `01-glossary.ko.md#clientserver-channel` | `../01-glossary.ko.md#clientserver-channel` |
| `01-glossary.ko.md#classic-fanout` | `../01-glossary.ko.md#classic-fanout` |
| `01-glossary.ko.md#stream-session` | `../01-glossary.ko.md#stream-session` |
| `01-glossary.ko.md#shutdown` | `../01-glossary.ko.md#shutdown` |
| `01-glossary.ko.md#deadline` | `../01-glossary.ko.md#deadline` |
| `01-glossary.ko.md#ready` | `../01-glossary.ko.md#ready` |
| `01-glossary.ko.md#liveness와-liveness-beacon` | `../01-glossary.ko.md#liveness와-liveness-beacon` |
| `01-glossary.ko.md#descriptor` | `../01-glossary.ko.md#descriptor` |
| `01-glossary.ko.md#lifecycle-generation` | `../01-glossary.ko.md#lifecycle-generation` |
| `01-glossary.ko.md#owner-lease` | `../01-glossary.ko.md#owner-lease` |
| `01-glossary.ko.md#reply-correlation` | `../01-glossary.ko.md#reply-correlation` |
| `01-glossary.ko.md#snapshot` | `../01-glossary.ko.md#snapshot` |
| `31-failure-failover-policy.ko.md#44-instance-spot-cold-activation과-owner-장애를-구분한다` (49 §1에서 인용) | `../31-failure-failover-policy.ko.md#44-instance-spot-cold-activation과-owner-장애를-구분한다` |
| `29-transport-liveness.ko.md#2-...`, `#3-...`, `#1-...` (49 §1에서 서로 인용, 두 문서가 합쳐지며 문서 내부 anchor로 전환) | 문서 안 절 번호(§1~§3)로 흡수, 별도 링크 없음 |

이 문서 자체가 옛 `29-transport-liveness.ko.md`를 대체하므로, 캠페인 마지막 단계에서
`29-…`를 참조하던 외부 파일 17개(§1 표 기준)와 `49-…` §1을 참조하던 파일들을 이 새 경로로
치환해야 한다. 07·08·09·10처럼 절 제목이 바뀌었으므로(절 번호는 29 원문과 다르게
재배치했다 — 예: 옛 §8 "관측 정보"가 새 §9로, 새 §8 "Liveness 판정은 authority를 바꾸지
않는다"가 49 §1에서 왔다) anchor 치환표가 필요하다.

## spec-gap 후보

- **G-candidate 1(mapping §7 그대로 인계)**: §4의 수신 독점 방지 상한 3축(건수·byte·경과
  시간)의 실제 값이 스펙에 없다. 원문이 명시하는 판정 가능 범위는 "무한하지 않다"와
  "회전 시작점이 이동한다"뿐이다(R127~R128). 새 값을 만들지 않았고, 검증 요구 절에도
  판정 불가로만 적었다. 4언어 구현 대조 단계에서 각 언어가 실제로 쓰는 값을 비교해
  하나로 정할지, 언어별 재량으로 남길지 판단이 필요하다.
