# `45-internal-routing-and-cache.ko.md` §3~§7 흡수 대장

`03-spot-actor` 주제의 [mapping.ko.md §3.1](../03-spot-actor/mapping.ko.md#31-18-object-routing--45-internal-routing-and-cache-병합-판정)이
`45`의 §1·§1.1·§2만 `08-routing.ko.md`(18-object-routing)로 병합하고, §3~§7은
"02-channel-transport 주제 소관"으로 넘겼다. 이 대장은 그 §3~§7을
`02-channel-messaging.ko.md`·`01-channel-topology.ko.md`로 흡수한 결과를 규칙 단위로 기록한다.

기존 문서에 R# 표(mapping.ko.md §5.1·§5.2)로 등재된 규칙과 중복되는 것은 "새 위치" 열에
"이미 존재"로 적고, 이번에 실제로 옮긴 문장만 새로 추가했다.

## §4 처리 방침

**`45` §4(선택을 어느 계층이 하는가)는 `02-channel-messaging.ko.md`로 넣었다.** 45의 자체 계약
소유 선언("선택 순서와 tiebreak는 Channel 메시징이 소유한다")이 §4·§5를 함께 가리키고, §4의
결론(정식 경로 둘은 framework가 고른다, Core는 connection 집합만 관리한다)이 바로 앞 §3의
`ChannelName select-one` 선택 절차가 왜 framework 책임인지 설명하는 이유이기 때문이다.
`01-channel-topology.ko.md`는 물리 연결·discovery·ready 상태를 다루지만 "누가 대상을
고르는가"라는 §4의 질문 자체에는 답하지 않으므로 그 문서로 옮기지 않았다. §4의 절반(수동 연결
fallback, ClientServer 전용 eligibility 표)은 `09-client-server-channel`(새 이름
`03-client-server-channel.ko.md`) 소관이며 그 문서는 이미 재작성이 끝나 이번 작업 범위 밖이다
— 아래 표에서 해당 행은 "이미 존재"로 표시했다.

## 대장

| 45의 절 | 규칙 | 새 위치 | 비고 |
|---|---|---|---|
| §3 | 후보 목록은 peer 상태가 바뀔 때만 다시 만들고, 호출 경로는 읽기만 한다 | `02-channel-messaging.ko.md` §3 "후보 목록과 선택 순서는 변경 시점에 미리 준비한다" | 신규. 규칙 문장 + 이유(peer 수 비례 비용 vs peer 상태 변경 빈도)로 재작성 |
| §3 | 호출마다 전체 peer를 훑으면 peer 수에 비례하는 비용이 모든 호출에 발생한다(이유 문장) | 위와 같음 | 신규, 이유 불릿에 흡수 |
| §4 | 대상 지정 방식·고르는 계층 표(MeshNode→framework, ClientServer→framework, 수동 연결 fallback→Core) | `02-channel-messaging.ko.md` §3 "Framework가 target을 고르고 Core는 연결을 관리한다" | 신규. 표를 산문 두 문단으로 재구성 |
| §4 | 정식 경로 둘(MeshNode, ClientServer)은 모두 framework가 고른다 | 위와 같음 | 신규 |
| §4 | framework는 socket 하나에 속한 connection 집합을 Core 대신 관리하지 않는다 — 후보 endpoint와 weight만 전달하고 Core가 연결 시점·재연결·송신 connection을 결정한다 | 위와 같음 | 신규, 굵은 규칙 문장 + 이유 |
| §4 | 경계를 넘으면 3가지가 함께 중복된다(연결 수명, socket·fd·monitor 자원, 연결 순서 무의미) | 위와 같음 | 표를 규칙 문단의 이유 절에 산문으로 흡수 |
| §4 | 연결 순서로 Core의 선택을 유도하려 하면 안 된다(함정) — winner를 앞에 회전해도 순서가 사라진다 | 위와 같음 | 신규, 굵은 규칙 문장 |
| §4 | 후보마다 socket 하나씩 만들어 framework가 고르는 구조는 겉보기엔 성립하지만 연결 수명·재연결 부담이 있다 | 위와 같음 | 신규, 이유 문단에 흡수 |
| §4 | 판정 기준(하위 계층이 eligibility·weight·안정적 식별자를 알고 강제할 수 있는가) + ClientServer 조건 표(ready·drain, descriptor identity·세대 일치, 수동 연결 검증, Server RID tiebreak) | — | **남김.** `03-client-server-channel.ko.md` §4·§4.1·§4.2가 이미 같은 내용을 다른 문장(Ready/drain 비교, local Server 후보 규칙, descriptor revision)으로 서술한다 — 그 문서는 이미 재작성이 끝나 이번 작업 범위 밖이라 중복 추가하지 않음 |
| §4 | 이 조건을 하위 계층에 투영하는 projection API가 없으면 지금은 per-server 연결과 framework 선택이 맞다 | — | **남김.** 아직 존재하지 않는 API에 대한 로드맵 메모이며 현재 공개 계약이 아니다. spec-gap 후보로 기록 |
| §4 | Core가 고르는 경로(수동 연결 fallback)에서 framework 계약을 만족하려면 Core의 load balancer가 §5 순서를 내야 한다 | `02-channel-messaging.ko.md` §3 "Framework가 target을 고르고 Core는 연결을 관리한다" 마지막 문단 | 신규, 다만 "Core가 아직 이 순서를 내는지"는 확인하지 않음 — spec-gap 후보로 별도 기록 |
| §5 | 가중치를 매끄럽게 분산하는 순환(SWRR)을 쓴다, 정식 spec이 계약으로 고정 | `02-channel-messaging.ko.md` §3 "가중 라운드로빈 선택 순서" | 이미 존재(재작성 시 반영됨) |
| §5 | 정식 spec 요구 2가지(장기 비율 수렴, ClientServer 같은 weight 순환) | 위와 같음 | 이미 존재 |
| §5 | 절차(누적값 유지, 3단계, tie-break) | 위와 같음 | 이미 존재 |
| §5 | 이 절차가 내는 결과(B,A,B,B 예시, 같은 weight 번갈아 선택) | 위와 같음 | 이미 존재 |
| §5 | 후보 순서를 topology별 식별자로 정렬해 둔다(RouteMesh: NodeRid, ClientServer: Server RID) + 연결 경로·등록 출처를 식별자로 쓰면 안 되는 이유 | 위와 같음 | 이미 존재 |
| §5 | 무작위를 쓰지 않는 이유(가중 무작위는 장기 비율은 맞지만 순환을 보장하지 않는다) | — | **남김.** 기존 §3에 없음. 알고리즘을 고정하는 이유는 이미 재현성 문장으로 설명되어 있어 새로 추가하지 않음 — 아래 "남긴 것" 참고 |
| §5 | 절차를 지키면서 호출 비용을 낮추는 방법 — 후보 목록이 바뀔 때 순서를 미리 계산해 두고 호출은 cursor만 옮긴다 | `02-channel-messaging.ko.md` §3 "후보 목록과 선택 순서는 변경 시점에 미리 준비한다" | 신규 |
| §5 | 절차가 결정적이므로 같은 누적값 상태가 다시 나타나면 그 사이가 주기다(도입부/주기 분리 저장) | 위와 같음 | 신규 |
| §5 | 단정 금지 1 — weight 합÷최대공약수는 누적값이 전부 0일 때만 주기다 | 위와 같음 | 신규, 내부 확인 조건 |
| §5 | 단정 금지 2 — 시작 상태 복귀를 기다리면 안 된다 | 위와 같음 | 신규, 내부 확인 조건 |
| §5 | 예시(A=-2,B=1,C=1에서 B 제외, 주기 2걸음) | — | **남김.** 서술을 위한 예시일 뿐이고 위 두 "단정 금지" 규칙만 옮기면 충분해 예시 자체는 옮기지 않음(가이드 §2.5, 없는 보장을 만들지 않기 위해 예시 재현 대신 원칙만 기술) |
| §5 | 주기 탐색에는 걸음 수·시간 두 상한, 상한 초과 시 호출마다 수행으로 복귀, 탐색은 후보 변경 경로에서만 | 위와 같음 | 신규, 내부 확인 조건 |
| §5 | 누적값 상태와 cursor 증가는 하나의 순서로 정렬, 단일 cursor 동기화 비용 대신 channel별 단일 경로 사용, shard별 독립 상태는 결과 순서가 갈려 계약 미충족 | 위와 같음 | 신규, 내부 확인 조건 |
| §5 | 후보 배열·정렬·집합 생성은 호출 경로에 두지 않는다 | 위와 같음 | 신규, 내부 확인 조건 |
| §6 | 이름만 지정 vs 직접 지정(node RID·객체 ID) 표 | `02-channel-messaging.ko.md` §5 "선택 뒤 자동 재전송하지 않는 이유" | 신규(Channel 대상만). 객체 ID(Spot·Actor) 직접 지정 예시는 이 문서 범위 밖이라 가져오지 않음 |
| §6 | "대상을 바꾸지 않는다"와 "호출이 성공한다"는 다른 보장 — 직접 지정한 대상이 준비되지 않았으면 실패로 끝나고 다른 후보로 옮기지 않는다 | 위와 같음 | 신규 |
| §7 | 발행을 시작할 때 대상 목록을 고정한다(snapshot) | `12-spot-messaging.ko.md` §4.2 | 이미 존재(03-spot-actor 주제가 이미 재작성). `02-channel-messaging.ko.md` §7에는 링크 문장만 강화 |
| §7 | 원격 node에는 message를 하나만 보내고 그 node가 자기 구독자에게 나눠 준다 | `12-spot-messaging.ko.md` §4.2 2단계 | 이미 존재. Channel 쪽에 새로 옮길 내용 없음 |
| §7 | 구독자마다 따로 보내면 전송량이 구독자 수에 비례(node당 100개면 100배), 나눠 주면 node 수에 비례(이유 문장) | — | **남김.** `12-spot-messaging.ko.md`에도 아직 이 구체적 이유 문장(수치 예시)이 없다 — 03-spot-actor 주제 몫으로 남김. 아래 "남긴 것" 참고 |
| §7 | 같은 node 안의 구독자에게는 각자의 대기열에 직접 넣는다 | `12-spot-messaging.ko.md` §4.2 5단계 | 이미 존재 |
| §7 | 일부 대상이 실패해도 이미 수락한 대상을 되돌리지 않는다(취소할 방법이 없다) | `12-spot-messaging.ko.md` §4.4 | 이미 존재 |
| §7 | 발행은 결과값 없이 완료하며 대상별 결과를 돌려주지 않는다, 완료 시점은 자기 자리를 확보한 때 | `12-spot-messaging.ko.md` §4.4 | 이미 존재 |
| §7 | 되돌리지도 알리지도 않으므로 발행은 "보냈다"까지만 보장 — 도달 확인이 필요하면 응답을 기다리는 호출을 쓴다 | `12-spot-messaging.ko.md` §4.4 | 이미 존재(취지상 동일). `02-channel-messaging.ko.md` §7의 링크 문장이 이 문서 전체를 가리키도록 앵커를 §4로 좁혀 강화 |

## 남긴 것과 그 이유

- **§4 ClientServer 전용 eligibility 표와 projection API 로드맵 메모.** eligibility 표는
  `03-client-server-channel.ko.md` §4·§4.1이 이미 같은 계약을 다른 문장으로 서술하고 있어
  옮기면 중복이다. projection API 메모는 아직 존재하지 않는 API를 전제로 한 내부 로드맵
  기록이라 현재 공개 계약 문서에 넣을 대상이 없다 — spec-gap 후보로만 남긴다.
- **§5 "무작위를 쓰지 않는 이유".** 알고리즘을 고정하는 이유(재현성, 언어 간 비교 가능)는
  이미 `02-channel-messaging.ko.md` §3의 "같은 후보 목록과 같은 누적값 상태에서는 항상 같은
  순서가 나온다"는 문장이 결과로 보여준다. "가중 무작위와 비교했을 때 왜 안 되는가"라는 대조
  설명은 규칙이 아니라 알고리즘 선택 배경이라 판단해 옮기지 않았다. 규칙 자체(순환 보장)는
  이미 완전히 반영되어 있어 대조 설명 없이도 대장 항목이 누락되지 않는다.
- **§7 "구독자마다 따로 보내면 100배"류의 구체적 수치 이유 문장.** `12-spot-messaging.ko.md`
  §4.2가 메커니즘(node마다 한 번, node가 나눠 준다)은 이미 규칙으로 갖고 있지만 그 이유를
  숫자로 뒷받침하는 문장은 없다. 이 문서는 `12-spot-messaging.ko.md`를 소유 문서가 아니라
  참조만 하므로 그 문서를 고치지 않았다 — 03-spot-actor 주제가 `45`를 완전히 비울 때 함께
  가져가야 할 항목이다.
- **`45` 문서 자체.** §3~§7을 모두 흡수해도 파일을 삭제하거나 "재작성 중" 표시를 추가하지
  않았다 — README.ko.md §3.1이 이미 "45가 완전히 빈 문서가 될 때까지는(§3~§6이 다른 주제로
  옮겨질 때까지는) 파일 자체를 삭제하지 않는다. 파일 삭제·이동은 마지막 단계(§5)의 몫이다"라고
  못 박았다.

## spec-gap 후보

- **Connection projection API 미존재.** `45` §4가 전제하는 "framework의 선택 정보를 하위
  계층에 전달하는 projection API"는 현재 어떤 공개 계약 문서에도 없다. ClientServer의
  per-server connection과 RouteMesh의 socket 하나당 하나 구조가 유지되는 근거이므로, 이
  구조를 바꾸려면 먼저 이 API를 공개 계약 절차로 확정해야 한다 — 지금은 기록만 하고 새 문서에
  계약으로 적지 않았다.
- **Core load balancer가 §5 SWRR 절차를 실제로 내는지 미확인.** `45` §4 "확인할 것"은 수동
  연결 fallback 경로의 계약 충족 여부가 "Core의 load balancer가 이 순서를 내는가"에 달려
  있다고 밝히지만, 이번 흡수 작업은 Core 구현을 대조하지 않았다. 04.4 구현 대조 단계에서
  Core 쪽 load balancer 알고리즘을 확인해야 한다.
- **`12-spot-messaging.ko.md`의 fanout 전송량 이유 문장 누락.** 위 "남긴 것"에 적은 대로,
  "구독자마다 따로 보내면 node당 구독자 수에 비례해 전송량이 늘어난다"는 이유 설명이 그
  문서에 아직 없다. 03-spot-actor 주제가 `45`를 완전히 비울 때 이 이유 문장을 §4.2에 추가할지
  판단해야 한다.
