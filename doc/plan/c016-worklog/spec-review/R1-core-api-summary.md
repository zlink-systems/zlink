# R1-core-api 스펙 심층 리뷰

| 번호 | 제목 | 분류 | 행동 변경 | 규칙 수 | 성능 영향 | 확신 |
|---|---|---|---|---|---|---|
| F-R1-1 | Pair 종료 뒤 correlation 유지 문장 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-2 | REQUEST 대기 토큰의 wake 조건 중복 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-3 | PUB·XPUB의 동일 publish 실패 경계 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-4 | XPUB topic buffer 부족 시 event 보존 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-5 | SUB·XSUB의 빈 topic 예외 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-6 | XPUB RID view의 수명 소유자 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-7 | 소비된 message의 초기화 상태 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-8 | Close의 내부 정리와 completion 전달 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-9 | 공유 completion 용량의 API별 오류 | consolidation | 있음 | 2 → 1 | 없음 | 높음 |
| F-R1-10 | REJECT의 reciprocal duplicate 예외 | spec-impl-drift | 있음 | 3 → 2 | 없음 | 높음 |
| F-R1-11 | Receive owner 충돌의 result 불일치 | spec-impl-drift | 있음 | 2 → 1 | 없음 | 중간 |
| F-R1-12 | 제출하지 않은 나머지 part의 소비 서술 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-13 | Public MORE staging의 HWM 경계 | spec-impl-drift | 있음 | 2 → 1 | 있음 | 중간 |
| F-R1-14 | Payload 없는 callback queue의 잔존 서술 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-15 | Option 계약의 discovery handle | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-16 | Monitor 검증 절의 lock 요구 | form | 없음 | 3 → 1 | 없음 | 높음 |
| F-R1-17 | Endpoint 해제 검증의 transport 범위 | form | 없음 | 2 → 1 | 없음 | 높음 |
| F-R1-18 | DISCONNECTED의 수신 독립성 검증 누락 | form | 없음 | 1 → 1 | 없음 | 높음 |
| F-R1-19 | STREAM 영문 검증 항목의 결합 | form | 없음 | 2 → 2 | 없음 | 높음 |
| F-R1-20 | 저장·조회만 하는 pending 옵션 | complexity | 있음 | 2 → 0 | 없음 | 높음 |

검토일: 2026-09-06. 지정된 국문·영문 스펙 40개, 16,156행을 모두 읽었다. 빌드·test·benchmark는 실행하지 않았으며 아래 관측 예시는 실행 결과가 아니라 코드에서 도출한 contract test 입력이다. 기존 작업 파일은 변경하지 않았다.

규칙 수는 각 finding이 지목한 동일 결정의 경쟁 서술·예외를 센다. 국문·영문 번역 한 쌍은 하나로 세며, 의미를 추가하지 않는 링크와 공개 관측 검증은 별도 동작 규칙으로 세지 않는다. F-R1-16은 내부 기구를 재규정하는 위치 수, F-R1-18·19는 동작 규칙을 유지하는 검증 문서 정리다. README가 순서를 정하지 않은 `complexity`는 지정된 우선순위 분류 뒤에 두었다.

`행동 변경: 없음`은 이미 명시된 공통 계약과 현재 구현을 유지하는 문서 통합 제안이다. 계약 간 우선권이 불명확하여 코드 변경 가능성이 남은 F-R1-11·13은 `있음`으로 보수적으로 분리했다. `있음` 항목은 현재 적용 대상이 아니며 0.18.0 판단 대상이다. 구현 대조 언어는 **공개 C ABI를 구현하는 Core C++**뿐이다. C++ binding을 포함한 각 언어 binding과 Framework 구현은 R3~R8 범위이므로 확인하지 않았고, 언어 간 동작 parity가 확인됐다는 뜻으로 읽어서는 안 된다.

### F-R1-1 Pair 종료 뒤 correlation 유지 문장

- 분류: consolidation
- 위치: `core/doc/spec/core/socket/README.ko.md:159-173,873-879,1082-1084,1157`; `core/doc/spec/core/socket/README.en.md:164-186,930-939,1187-1189,1277`; `core/doc/spec/core/socket/06-dealer.ko.md:350-353,438-439`; `core/doc/spec/core/socket/06-dealer.en.md:371-375,467-469`; `core/doc/spec/core/socket/07-router.ko.md:224-228,467`; `core/doc/spec/core/socket/07-router.en.md:237-241,490`. 결정 근거: `doc/plan/c016-worklog/decisions.ko.md:1205-1211,1235-1241`(D-090·091).
- 현재 규칙(인용): “payload를 replay하지 않고 correlation과 이미 시작한 budget만 유지한다.” / “Core가 그 pair 종료 즉시 `ZLINK_REQUEST_NOT_CONNECTED`(`EHOSTUNREACH`)로 정확히 한 번 종결”.
- 문제: Request timeout 설명과 DEALER·ROUTER 설명에는 disconnect 뒤 correlation을 유지하는 이전 규칙이 남아 있다. 공통 completion 표와 REJECT·HANDOVER 설명은 이미 submit 시점 pair 종료를 즉시 종결 원인으로 삼는다. 구현은 종료 시 pending을 제거하고 correlation을 반환한다. 개별 socket 검증의 “replay·budget reset 없음”만으로는 timeout까지 기다리는 잘못된 구현도 통과할 수 있다. 명시적 local endpoint·RID 제거의 NOT_FOUND는 공통 표의 별도 원인으로 유지한다.
- 제안: 소유자는 `socket/README` §6의 completion 원인 표로 통합하고 다른 설명은 해당 행을 참조한다: **“명시적 local endpoint·RID 제거가 아닌 submit 시점 transport pair 종료는 transient disconnect·HANDOVER·REJECT 등 원인에 관계없이 admit된 REQUEST를 종료 즉시 `ZLINK_REQUEST_NOT_CONNECTED`로 한 번 종결하며, 이후 reply는 추가 completion을 만들지 않는다.”**
- 규칙 수: before 2 → after 1 — disconnect 후 budget 유지와 pair 종료 시 즉시 종결을 단일 종결 규칙으로 합친다.
- 행동 변경: 없음 — D-090·091과 현재 Core의 즉시 종결 동작을 유지한다.
- 영향: core — `core/src/runtime/sockets/common/socket_base_api.cpp:1729-1734`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — 문서 통합이며 request hot path의 timer·lock·상태를 바꾸지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/runtime/sockets/common/socket_base_api.cpp:1729-1734`; `core/src/api/socket/socket_request_reply_dispatch.cpp:401-419,455-468`; `core/src/runtime/sockets/router/router_admission.cpp:436-449`.
- 확신: 높음 — 최신 결정, 공통 completion 표, 실제 pending 제거 경로가 일치한다.

### F-R1-2 REQUEST 대기 토큰의 wake 조건 중복

- 분류: consolidation
- 위치: `core/doc/spec/core/socket/README.ko.md:285,982-996,1067-1076,1151,1157,1331-1346,1358-1362`; `core/doc/spec/core/socket/README.en.md:300,1055-1081,1166-1181,1271,1277,1514-1535,1552-1557`; `core/doc/spec/core/05-polling.ko.md:60-67,79-82,302-305`; `core/doc/spec/core/05-polling.en.md:66-74,87-92,321-324`; `core/doc/spec/core/socket/06-dealer.ko.md:105-109,196-197,273-275,297-307,342-348,401-403,416-419,434-437,473`; `core/doc/spec/core/socket/06-dealer.en.md:109-114,209-210,292-294,318-327,363-369,427-429,443-446,463-466,510`; `core/doc/spec/core/socket/07-router.ko.md:149-151,178-184,219-221,423-425,457-459,464`; `core/doc/spec/core/socket/07-router.en.md:157-159,187-194,231-235,448-450,480-482,487`. 결정 근거: `doc/plan/c016-worklog/decisions.ko.md:1277-1280,1294-1298`(D-B119·120).
- 현재 규칙(인용): “Target에 write credit이 생기면” / “거절 원인이 되는 자원의 회복만 wake 조건이다 — 규칙 하나”.
- 문제: 좁은 REQUEST 문단만 correlation reservation 반환을 조건으로 삼고, 앞뒤 문장·enum 주석·공통 표·개별 socket 문장은 physical credit, attach, RESUME, weight 변경을 무조건적인 wake로 읽히게 한다. 특히 공통 표의 pair 종료 후 reconnect wake도 correlation 거절 토큰에 그대로 적용하면 틀린 설명이다. 구현은 correlation wait를 physical credit 알림에서 제외하며 reservation 반환 epoch를 확인한다. SEND 전용 설명 자체가 틀렸다는 지적은 아니다. SEND 설명을 REQUEST에도 무조건 적용하는 재서술이 원인이다.
- 제안: 소유자는 `socket/README` §6의 대기 토큰 공통 규칙으로 정하고 polling·각 socket은 참조한다: **“SEND·REQUEST의 대기 토큰은 그 제출을 거절한 자원이 회복될 때만 WRITABLE을 발행하며, 다른 자원의 회복과 socket 수준 readiness는 그 토큰의 발행 조건을 대신하지 않는다.”**
- 규칙 수: before 2 → after 1 — 일반 physical-credit wake와 correlation 전용 예외를 거절 자원 회복 하나로 표현한다.
- 행동 변경: 없음 — D-B120의 원인별 wake 동작을 유지한다.
- 영향: core — `core/src/runtime/sockets/common/socket_send_complete.cpp:199-228`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — 문서 변경이며 이미 제거된 spurious WRITABLE의 성능 효과를 새 효과로 계산하지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_request_reply_submit_api.cpp:114-123`; `core/src/runtime/sockets/common/socket_send_complete.cpp:199-228`; `core/src/api/socket/socket_completion_queue_internal.cpp:381-395`; `core/src/runtime/core/pipe.cpp:1606-1611,1625-1658`.
- 확신: 높음 — 거절 원인 보존과 두 wake 경로의 배타 처리가 코드에 있다. 검증에는 correlation-full 상태에서 physical 회복만 발생해도 WRITABLE이 없고 reservation 반환 뒤에만 한 건이 생기는 관측이 필요하다.

### F-R1-3 PUB·XPUB의 동일 publish 실패 경계

- 분류: consolidation
- 위치: `core/doc/spec/core/socket/02-pub.ko.md:64-74,88-93,208-216,246-247`; `core/doc/spec/core/socket/02-pub.en.md:67-80,94-99,225-236,269-270`; `core/doc/spec/core/socket/04-xpub.ko.md:168-185,278-279`; `core/doc/spec/core/socket/04-xpub.en.md:156-169,256-257`.
- 현재 규칙(인용): PUB의 “제출 전 검증에서 실패하면 호출한 `part_`만 소비하고 열린 sequence는 유지한다.” / XPUB의 “열린 sequence의 중간 또는 마지막 submit이 실패하면 이전에 staging한 part와 실패한 part를 원자적으로 폐기하고 sequence를 닫는다.”
- 문제: 두 문서는 같은 public 함수 `zlink_publish_part`와 같은 PUB·XPUB 적용 범위를 각각 규정한다. PUB은 sequence 사전 검증 실패와 실제 send 실패를 구분하지만 XPUB은 모든 실패를 abort로 규정하고 검증 절에서도 반복한다. 실제 함수는 두 socket을 같은 경로로 처리하고 publish의 사전 검증 실패에서는 기존 sequence를 남긴다. Topic을 바꾼 잘못된 part를 제출한 뒤 원래 topic으로 FINAL을 제출하면 이 차이를 공개 수신으로 관찰할 수 있다.
- 제안: 소유자는 `socket/02-pub` §3으로 두고 XPUB의 함수 설명과 검증은 해당 공통 동작을 참조한다: **“PUB·XPUB의 `zlink_publish_part`는 sequence 사전 검증 실패에서는 호출 part만 소비하고 열린 record를 유지하며, 검증을 통과한 send 실패에서는 제출된 record 전체를 폐기한다.”**
- 규칙 수: before 2 → after 1 — 같은 함수의 PUB형·XPUB형 실패 경계를 합친다.
- 행동 변경: 없음 — PUB 문서에 이미 명시되고 두 type의 공통 구현이 제공하는 동작을 유지한다.
- 영향: core — `core/src/api/socket/socket_message_send_api.cpp:304-325`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — publish staging·복사·admission을 변경하지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_message_send_api.cpp:304-325,714-767`; `core/src/api/socket/part_helper_api.cpp:130-149,781-800`.
- 확신: 높음 — 동일 API 진입점과 실패 단계별 cleanup 분기가 직접 확인된다.

### F-R1-4 XPUB topic buffer 부족 시 event 보존

- 분류: consolidation
- 위치: `core/doc/spec/core/socket/04-xpub.ko.md:220-234,267`; `core/doc/spec/core/socket/04-xpub.en.md:200-212,243`; `core/doc/spec/core/socket/README.ko.md:596-600,1314-1318`; `core/doc/spec/core/socket/README.en.md:640-647,1486-1492`; `core/doc/spec/core/03-errors.ko.md:391-392,563-565`; `core/doc/spec/core/03-errors.en.md:408-409,600-603`.
- 현재 규칙(인용): “`errno = EMSGSIZE`로 실패한다. 이때 구독 event는 이미 queue에서 dequeue되었으며, 충분한 buffer로 같은 event를 다시 수신할 수 없다.”
- 문제: XPUB 개별 문서는 부족한 buffer를 `INTERNAL_ERROR/EMSGSIZE`와 event 유실로 설명한다. 공통 typed receive 계약은 XPUB도 `BUFFER_TOO_SMALL/ENOBUFS`와 record 보존 대상이라고 명시한다. 구현은 가져온 event를 helper state에 보존하고 충분한 buffer로 다시 받으면 같은 event를 돌려준다. 내부 pipe에서 먼저 읽었는지와 public record를 소비했는지를 혼동한 이전 설명이 검증 절까지 남았다.
- 제안: 소유자는 `socket/README`의 typed receive buffer 계약이다: **“SUB·XSUB·XPUB의 topic buffer가 실제 topic 길이보다 작으면 필요 길이만 반환하고 `ZLINK_RECV_BUFFER_TOO_SMALL/ENOBUFS`로 record를 보존하여, 충분한 buffer의 다음 수신이 같은 record를 한 번 반환한다.”**
- 규칙 수: before 2 → after 1 — XPUB 전용 유실 규칙을 공통 비소비 규칙에 합친다.
- 행동 변경: 없음 — 공통 계약과 현재 XPUB 구현을 유지한다.
- 영향: core — `core/src/api/socket/socket_message_recv_api.cpp:350-420`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — 현재의 보존 storage·수신 복사 횟수는 그대로다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_message_recv_api.cpp:326-386,397-420`; `core/src/api/message/recv_result_internal.hpp:29-30`.
- 확신: 높음 — ENOBUFS 반환 전에 state를 해제하지 않으며 성공 경로에서만 record를 완료한다.

### F-R1-5 SUB·XSUB의 빈 topic 예외

- 분류: consolidation
- 위치: `core/doc/spec/core/socket/03-sub.ko.md:203-210,280-281`; `core/doc/spec/core/socket/03-sub.en.md:203-209,294-299`; `core/doc/spec/core/socket/05-xsub.ko.md:192-199,280-281`; `core/doc/spec/core/socket/05-xsub.en.md:203-211,328-335`; `core/doc/spec/core/socket/README.ko.md:596-600,1314-1318`; `core/doc/spec/core/socket/README.en.md:640-647,1486-1492`.
- 현재 규칙(인용): “`topic_id_capacity_ == 0`이거나 topic을 담기에 작으면” / “길이 0 topic은 capacity 0·NULL buffer로 성공하고 record를 소비한다.”
- 문제: SUB·XSUB의 함수 설명과 검증은 capacity 0을 topic 길이와 무관한 실패 조건으로 추가한다. 공통 계약과 실제 수신 함수는 capacity와 실제 길이만 비교하므로 빈 topic은 capacity 0으로 성공한다. F-R1-4의 XPUB 유실 문제와 별개로, 여기서는 불필요한 zero-capacity 특례 하나가 원인이다.
- 제안: 소유자는 `socket/README`의 typed receive buffer 계약이며 SUB·XSUB의 별도 zero 조건을 삭제한다: **“Topic buffer의 용량 부족은 `capacity < topic_length`일 때만 성립하므로 길이 0 topic은 capacity 0·NULL buffer로 정상 수신된다.”**
- 규칙 수: before 2 → after 1 — zero 무조건 실패와 길이 비교를 길이 비교 하나로 합친다.
- 행동 변경: 없음 — 현재 구현과 명시된 공통 빈 topic 계약을 유지한다.
- 영향: core — `core/src/api/socket/socket_message_api.cpp:435-450`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — 실제 코드에는 삭제할 zero 특례가 없다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_message_api.cpp:435-450,538-550`; `core/src/api/message/recv_result_internal.hpp:29-30`.
- 확신: 높음 — 단일 part와 buffered multipart 양쪽에서 동일한 길이 비교를 사용한다.

### F-R1-6 XPUB RID view의 수명 소유자

- 분류: consolidation
- 위치: `core/doc/spec/core/socket/04-xpub.ko.md:209-218,265`; `core/doc/spec/core/socket/04-xpub.en.md:192-198,241`; `core/doc/spec/core/socket/README.ko.md:541-545,588-592,1309-1310`; `core/doc/spec/core/socket/README.en.md:579-584,629-633,1479-1481`. 동일 공통 수명의 ROUTER·STREAM 재서술: `core/doc/spec/core/socket/07-router.ko.md:48-50,273-274`; `core/doc/spec/core/socket/07-router.en.md:50-53,287-288`; `core/doc/spec/core/socket/08-stream.ko.md:156-158,209-213,456-457`; `core/doc/spec/core/socket/08-stream.en.md:164-168,220-225,490-492`.
- 현재 규칙(인용): “이 storage는 socket별이 아니라 호출 thread별로 공유되므로” / “같은 socket의 다음 data recv는 성공 여부와 관계없이 진입 시 이전 view를 무효화한다.”
- 문제: XPUB만 같은 thread의 어느 XPUB에서든 다음 성공 호출까지라고 설명한다. 실제 RID storage는 각 socket의 endpoint runtime에 있으며 해당 socket의 data receive 진입 시 지운다. 따라서 다른 XPUB의 수신은 원래 view를 바꾸지 않고, 같은 XPUB의 실패한 수신도 이전 view를 무효화한다. Storage 소유자와 무효화 경계를 개별 문서가 공통 계약과 다르게 재정의한 것이 원인이다.
- 제안: 소유자는 `socket/README`의 borrowed RID 문단이며 개별 socket에는 링크만 남긴다: **“Core-owned RID view는 이를 반환한 socket의 다음 data receive API 진입 또는 close까지 유효하며, 다른 socket의 receive와 poller·completion·monitor 호출은 그 수명에 영향을 주지 않는다.”**
- 규칙 수: before 2 → after 1 — thread별 성공 호출 수명과 socket별 진입 수명을 후자로 통합한다.
- 행동 변경: 없음 — 공통 계약과 현재 per-socket storage를 유지한다.
- 영향: core — `core/src/runtime/sockets/common/socket_endpoint_runtime.cpp:53-71`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — RID 저장·복사·초기화 경로를 변경하지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_message_recv_api.cpp:293-296,344-345,415-418`; `core/src/runtime/sockets/common/socket_base_dispatch.cpp:356-375`; `core/src/runtime/sockets/common/socket_endpoint_runtime.cpp:53-71`.
- 확신: 높음 — public 함수의 무효화 호출과 실제 storage의 소유 객체가 모두 확인된다.

### F-R1-7 소비된 message의 초기화 상태

- 분류: consolidation
- 위치: `core/doc/spec/core/socket/01-pair.ko.md:101-103,200`; `core/doc/spec/core/socket/01-pair.en.md:102-103,199`; `core/doc/spec/core/socket/02-pub.ko.md:218-220,243`; `core/doc/spec/core/socket/02-pub.en.md:238-241,266`; `core/doc/spec/core/socket/04-xpub.ko.md:171-173,274`; `core/doc/spec/core/socket/04-xpub.en.md:160-161,251`. 현재 빈 초기화 상태의 공통·개별 서술: `core/doc/spec/core/socket/README.ko.md:935`; `core/doc/spec/core/socket/README.en.md:998-1000`; `core/doc/spec/core/02-message.ko.md:423-427,459`; `core/doc/spec/core/02-message.en.md:439-443,480-481`; `core/doc/spec/core/socket/06-dealer.ko.md:135-138,422`; `core/doc/spec/core/socket/06-dealer.en.md:144-147,450`; `core/doc/spec/core/socket/07-router.ko.md:58-61,289,445`; `core/doc/spec/core/socket/07-router.en.md:61-65,303,470`; `core/doc/spec/core/socket/08-stream.ko.md:435`; `core/doc/spec/core/socket/08-stream.en.md:466`.
- 현재 규칙(인용): “소비된 `zlink_msg_t`는 다시 초기화한 뒤에만 재사용할 수 있다.”
- 문제: PAIR·PUB·XPUB은 소비된 storage를 미초기화 상태처럼 설명한다. 공통 SEND와 DEALER·ROUTER 문서는 초기화된 길이 0 message라고 설명하며, 실제 소비 helper도 close 뒤 init한다. Payload를 다시 쓸 수 없다는 사실과 message 객체가 초기화되어 있다는 사실을 혼동한 것이다. 추가 init 없이 `zlink_msg_close`하거나 move/copy의 destination으로 쓰는 것은 이미 가능한 동작이다.
- 제안: 소유자는 `socket/README`의 part ownership 규칙이다: **“Send가 소비한 `part_`는 payload를 소유하지 않는 초기화된 빈 message로 남으므로 그대로 close하거나 초기화된 destination으로 재사용할 수 있으며, 원래 payload가 필요하면 제출 전에 별도 소유해야 한다.”**
- 규칙 수: before 2 → after 1 — 재초기화 필수와 이미 초기화됨이라는 충돌을 제거한다.
- 행동 변경: 없음 — 현재 message 객체의 반환 상태와 기존 공통 계약을 유지한다.
- 영향: core — `core/src/api/socket/request_reply_protocol_internal.hpp:111-125`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — Core 소비 helper를 바꾸지 않으며 application의 불필요한 init 감소를 측정 효과로 주장하지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/request_reply_protocol_internal.hpp:111-125`; `core/src/api/socket/socket_message_send_api.cpp:332-348,391-394`; `core/src/api/message/message_api.cpp:91-114`.
- 확신: 높음 — 반환 storage 상태가 코드와 여러 공통 계약에 명시된다.

### F-R1-8 Close의 내부 정리와 completion 전달

- 분류: consolidation
- 위치: `core/doc/spec/core/socket/README.ko.md:310-313,620-622,987-988,1002-1005,1047-1048,1074-1076,1158,1344-1346,1394-1396`; `core/doc/spec/core/socket/README.en.md:325-328,669-672,1063-1065,1083-1088,1136-1143,1175-1178,1278,1533-1535,1595-1598`; `core/doc/spec/core/03-errors.ko.md:374-375`; `core/doc/spec/core/03-errors.en.md:391-392`; `core/doc/spec/core/socket/06-dealer.ko.md:303-304`; `core/doc/spec/core/socket/06-dealer.en.md:324-325`; `core/doc/spec/core/socket/07-router.ko.md:182-184`; `core/doc/spec/core/socket/07-router.en.md:192-194`; `core/doc/spec/core/socket/08-stream.ko.md:240-245,475-477`; `core/doc/spec/core/socket/08-stream.en.md:252-257,513-515`.
- 현재 규칙(인용): “Nonzero wait token의 WRITABLE record는 정확히 한 번 반환되며(명시적 RID 제거·close에서는 `ZLINK_SEND_TERMINAL`)” / “새 completion 전달을 보장하지 않음”.
- 문제: Close 계약은 unread record와 진행 중 request를 내부 정리하고 public 전달을 보장하지 않는다. 반면 STREAM 검증과 공통 토큰 종료 설명은 close도 WRITABLE 반환을 받을 수 있는 종료처럼 서술한다. 구현은 waiter를 내부 terminal 상태로 만든 뒤 ready head와 public readiness를 지운다. 내부 retirement를 public exactly-once delivery와 같은 규칙으로 표현한 것이 원인이다. Close 뒤 내부 token의 terminal 값을 확인하라는 검증은 공개 pull만으로 작성할 수 없다.
- 제안: 소유자는 `socket/README`의 completion pull 수명 규칙이다: **“열린 socket의 completion은 ID마다 한 번 반환되지만 close·context termination은 미완료 및 unread record를 내부 정리하고 추가 전달을 보장하지 않으므로 필요한 결과와 payload는 close 전에 수신한다.”**
- 규칙 수: before 2 → after 1 — 모든 종료의 전달 약속과 lifecycle 폐기 계약을 수명 조건이 있는 전달 규칙으로 합친다.
- 행동 변경: 없음 — 이미 명시된 close 계약과 현재 queue 폐기 동작을 유지한다.
- 영향: core — `core/src/api/socket/socket_completion_queue_internal.cpp:529-572`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — close의 queue 순회·내부 retirement를 삭제하자는 제안이 아니다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_completion_queue_internal.cpp:529-572`; `core/src/runtime/sockets/common/socket_base_api.cpp:172-189`; `core/src/api/socket/socket_request_reply_dispatch.cpp:471-497`.
- 확신: 높음 — public ready queue를 비우는 코드와 명시적 close 문장이 일치한다.

### F-R1-9 공유 completion 용량의 API별 오류

- 분류: consolidation
- 위치: `core/doc/spec/core/socket/README.ko.md:957,973-980,1054-1055,1338-1349`; `core/doc/spec/core/socket/README.en.md:1022,1042-1053,1147-1149,1524-1539`; `core/doc/spec/core/socket/01-pair.ko.md:195`; `core/doc/spec/core/socket/01-pair.en.md:194`; `core/doc/spec/core/socket/06-dealer.ko.md:440-442`; `core/doc/spec/core/socket/06-dealer.en.md:470-473`; `core/doc/spec/core/socket/07-router.ko.md:209-210,468`; `core/doc/spec/core/socket/07-router.en.md:219-221,491`; `core/doc/spec/core/03-errors.ko.md:339,349`; `core/doc/spec/core/03-errors.en.md:356,366`.
- 현재 규칙(인용): “Slot 포화는 flags와 무관하게 즉시 `ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, ID `0`, completion 없음이다.” / SEND 표의 “`ZLINK_SUBMIT_OUT_OF_MEMORY`, `ENOMEM`”.
- 문제: 동일 socket의 65,536개 공유 reservation 한도가 SEND에서는 메모리 부족, REQUEST에서는 capacity backpressure가 된다. 실제 reservation 소유자는 한 함수이며 한도 초과와 allocator 실패를 이미 구분한다. WRITABLE 예약 wrapper가 SEND 계약을 맞추려고 EAGAIN을 ENOMEM으로 다시 분류한다. 이 차이는 구현 결함이 아니라 현재 계약이 요구하는 추가 규칙이다.
- 제안: 0.18.0의 소유자는 `socket/README`의 공유 completion reservation 문단으로 정한다: **“공유 completion reservation 한도에 도달한 제출은 API 종류와 무관하게 `BACKPRESSURED/EAGAIN`, ID 0, completion 없음으로 끝나며 실제 할당 실패만 `OUT_OF_MEMORY/ENOMEM`으로 구분한다.”**
- 규칙 수: before 2 → after 1 — 공유 capacity 거절 결과의 SEND·REQUEST별 변환을 제거한다.
- 행동 변경: 있음 — SEND caller는 용량 포화에서 기존 OUT_OF_MEMORY 대신 BACKPRESSURED와 ID 0을 관찰하므로 현행 계약으로 즉시 적용할 수 없다.
- 영향: core — `core/src/api/socket/socket_completion_queue_internal.cpp:237-260`; binding·Framework의 result 분기 영향은 언어별로 미확인.
- 성능 영향: 없음 — 포화 실패 경로의 errno 재분류만 사라지며 정상 send hot path의 복사·lock·예약 수는 같다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_completion_queue_internal.cpp:107-128,237-260`; `core/src/api/socket/socket_request_reply_pending_api.cpp:319-323`; `core/src/runtime/sockets/common/socket_send_complete.cpp:199-206`.
- 확신: 높음 — wrapper 주석도 SEND 공개 계약 때문에 변환한다고 명시한다. 실제 allocator 실패까지 합치자는 제안은 아니다.

### F-R1-10 REJECT의 reciprocal duplicate 예외

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/socket/README.ko.md:150-168,384`; `core/doc/spec/core/socket/README.en.md:155-179,401`. 결정 근거: `doc/plan/c016-worklog/decisions.ko.md:1281-1286,1375`(D-094·101).
- 현재 규칙(인용): “`ZLINK_RID_DUPLICATE_REJECT`는 기존 pipe를 유지하고 새 중복 pipe를 등록하지 않으며, 등록하지 않은 중복 pipe는 즉시 닫는다.”
- 문제: `adopt_peer_routing_id`의 `_handover`가 false여도 `paired_application && reciprocal_duplicate`이면 REJECT 분기를 건너뛴다. 이어지는 RID 비교에 따라 새 pipe를 standby로 등록하거나 기존 route를 물러나게 한다. D-094는 reciprocal collapse도 HANDOVER 아래에 두었으므로 이 분기는 REJECT/HANDOVER 외의 세 번째 admission 규칙이다. 공개 C API 관측 예시: RID `a < b`인 두 ROUTER를 REJECT로 bind하고 b→a 연결을 먼저 성립시킨 뒤 a→b를 추가하면, 새 반대 방향이 기존 pair를 supersede하는 경로에 들어가 기존 pair의 pending REQUEST를 NOT_CONNECTED로 종결할 수 있다. 이 예시는 실행하지 않았다.
- 제안: 소유자는 Core ROUTER admission이며 `socket/README` §4의 정책 문장을 유지한다: **“동일 RID의 새 pipe는 REJECT이면 방향과 관계없이 닫고 기존 pipe를 유지하며, HANDOVER에서만 같은 방향 교체 또는 reciprocal 방향 선택을 수행한다.”**
- 규칙 수: before 3 → after 2 — REJECT·HANDOVER·REJECT reciprocal 특례에서 두 정책만 남긴다.
- 행동 변경: 있음 — REJECT에서 새 반대 방향 연결의 채택·standby 등록과 기존 요청의 supersession 종료가 사라진다.
- 영향: core — `core/src/runtime/sockets/router/router_admission.cpp:343-374,393-417`; binding·Framework 언어별 구현 미확인이며 상위 보상 로직을 제안하지 않는다.
- 성능 영향: 없음 — 연결 admission의 예외 제거이며 정상 DATA·REQUEST record hot path의 검증·복사 수는 바꾸지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/runtime/sockets/router/router.cpp:35,81-91`; `core/src/runtime/sockets/router/router_admission.cpp:303-374,393-417,436-449`.
- 확신: 높음 — REJECT 설정이 `_handover=false`가 되는 경로와 이를 우회하는 조건, observable pending 종료까지 연결된다. 전송 시나리오 실측은 하지 않았다.

### F-R1-11 Receive owner 충돌의 result 불일치

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/socket/README.ko.md:529-532,588-590`; `core/doc/spec/core/socket/README.en.md:563-568,629-632`; `core/doc/spec/core/03-errors.ko.md:383,389,541`; `core/doc/spec/core/03-errors.en.md:400,406,567-569`.
- 현재 규칙(인용): “다른 thread나 family가 중간에 진입하면 `ZLINK_RECV_INVALID_STATE`, `errno == EBUSY`이고” / 오류 표의 “`ZLINK_RECV_BUSY`”.
- 문제: 공통 receive 문단은 multipart owner 충돌을 INVALID_STATE/EBUSY로 규정하지만 오류 표와 `from_errno(EBUSY)`는 BUSY다. SUB·XPUB의 owner 확인도 EBUSY를 이 변환에 넘긴다. 단순 번역 차이가 아니라 국문·영문 모두 안에서 계약이 충돌한다. 검증 절의 일반적인 result/errno 대응만으로는 어느 반환 enum을 보존해야 하는지 정해지지 않는다.
- 제안: 감독이 구체적인 receive 계약을 유지한다고 결정할 경우 소유자는 `03-errors`의 receive 결과 대응으로 통합한다: **“열린 multipart receive의 owner thread 또는 family를 위반한 호출은 `ZLINK_RECV_INVALID_STATE/EBUSY`를 반환하고 진행 중 record와 output을 보존한다.”**
- 규칙 수: before 2 → after 1 — 동일 owner 위반의 BUSY·INVALID_STATE 분류를 하나로 정한다.
- 행동 변경: 있음 — 위 초안을 구현하면 해당 호출에서 현재 BUSY가 INVALID_STATE로 바뀐다. 반대로 문서만 BUSY로 바꾸는 결정은 본 리뷰가 승인하지 않는다.
- 영향: core — `core/src/api/message/recv_result_internal.hpp:23-24`; binding·Framework의 enum 매핑·exception 분기는 언어별 미확인.
- 성능 영향: 없음 — 동일 실패에 대한 result 분류 문제이며 정상 receive의 lock·복사 제거를 주장하지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/api/message/recv_result_internal.hpp:23-34`; `core/src/api/socket/part_helper_api.cpp:680-684`; `core/src/api/socket/socket_message_api.cpp:383-387`; `core/src/api/socket/socket_message_recv_api.cpp:316-322`.
- 확신: 중간 — 불일치는 확실하지만 충돌하는 두 계약 중 보존할 enum은 감독 결정이 필요하다(BLOCKERS).

### F-R1-12 제출하지 않은 나머지 part의 소비 서술

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/02-message.ko.md:423-427,471`; `core/doc/spec/core/02-message.en.md:438-443,501-503`; part 단위 소유권의 소유 문장: `core/doc/spec/core/socket/README.ko.md:934-938`; `core/doc/spec/core/socket/README.en.md:998-1003`; `core/doc/spec/core/socket/06-dealer.ko.md:135-143,422`; `core/doc/spec/core/socket/06-dealer.en.md:144-153,450`; `core/doc/spec/core/socket/07-router.ko.md:58-61,289,445`; `core/doc/spec/core/socket/07-router.en.md:61-65,303,470`.
- 현재 규칙(인용): “send가 중간에 실패하면 실패한 part부터 나머지 part를 모두 close하고 빈 message로 다시 초기화하며”.
- 문제: `02-message`의 내부 transaction 설명과 공개 검증 문장이 private batch의 나머지 part 정리를 part 단위 public API에 그대로 투영한다. Public 호출은 `part_` 하나만 전달받으므로 application이 아직 제출하지 않은 후속 message storage에 접근할 수 없다. 내부 helper의 배열은 이미 제출되어 Core로 이동한 part들이다. 미래의 caller 소유 part까지 소비된다는 관측은 현재 인터페이스로 성립할 수 없다.
- 제안: 소유자는 `socket/README`의 part ownership 규칙이며 `02-message`는 이를 참조한다: **“실패한 part 호출은 그 호출에 전달된 part를 소비하고 해당 API의 abort 계약에 따라 이미 제출된 staging을 정리하며, 아직 제출하지 않은 caller 소유 part는 변경하지 않는다.”**
- 규칙 수: before 2 → after 1 — private batch의 나머지 배열 소비와 public part 소유권을 제출 여부 경계 하나로 표현한다.
- 행동 변경: 없음 — 현재 public API가 실제로 접근하는 storage 범위를 바꾸지 않는 내부 서술·검증 문장 정정이다.
- 영향: core — `core/src/api/socket/socket_message_send_api.cpp:332-348,418-434`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — 이미 제출된 배열의 cleanup이나 move 횟수는 그대로다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_message_send_api.cpp:332-348,391-394,418-434`; `core/src/api/socket/request_reply_protocol_internal.hpp:128-134`; `core/src/api/socket/part_helper_api.cpp:781-800`.
- 확신: 높음 — 단일 part public 인자와 Core 소유 배열의 경계가 명확하다. F-R1-3의 publish 사전 검증 예외를 이 문장으로 덮어쓰지 않는다.

### F-R1-13 Public MORE staging의 HWM 경계

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/socket/README.ko.md:428-442,935-938,1299-1301`; `core/doc/spec/core/socket/README.en.md:451-474,998-1003,1457-1462`; `core/doc/spec/core/socket/06-dealer.ko.md:140-143`; `core/doc/spec/core/socket/06-dealer.en.md:149-153`. Pipe HWM 본문은 physical pipe를 대상으로 시작하지만 첫 MORE 검증에는 public staging 제외 조건이 없다.
- 현재 규칙(인용): “최종 전체 크기를 모르는 incremental multipart에는 첫 `MORE` frame부터 일반 byte HWM을 적용하므로 frame이 제한 없이 누적되지 않는다.”
- 문제: Completion-aware public send의 MORE는 part를 socket-local 배열로 move하고 OK를 반환한다. Public send scope는 lifecycle/admission 소유권을 얻지만 MORE payload의 byte HWM을 검사하지 않는다. FINAL에서야 모인 배열로 whole-record admission을 시도한다. 따라서 FINAL이 오기 전까지 최종 크기를 모르는 public MORE들이 physical pipe의 incremental HWM 경로에 도달하지 않는다. PAIR에서 작은 유한 HWM과 그보다 큰 MORE payload를 주는 관측으로 경계를 구분할 수 있다. PUB·XPUB의 별도 publish 경로까지 같은 결함이라고 일반화하지 않았다.
- 제안: 첫 MORE 규정을 public API 계약으로 유지하는 경우 소유자는 `socket/README`의 HWM admission 절이다: **“Public part send에서 최종 크기를 알 수 없는 multipart는 첫 MORE부터 일반 byte HWM의 제한을 받고, FINAL까지의 비공개 staging을 이유로 그 제한을 벗어나지 않는다.”**
- 규칙 수: before 2 → after 1 — public staging 후 FINAL admission과 첫 MORE부터 bounded admission이라는 서로 다른 경계를 한 계약으로 정한다.
- 행동 변경: 있음 — 현재 OK인 일부 MORE가 FINAL 이전에 backpressure를 반환하게 된다. Physical pipe에만 적용한다는 문서 축소를 임의로 선택하지 않았다.
- 영향: core — `core/src/api/socket/socket_message_send_api.cpp:332-353,427-462`; binding·Framework의 multipart 제출 동작은 언어별 미확인.
- 성능 영향: 있음 — 거절될 MORE의 `buffered_parts.append_uninitialized`와 socket-local 보유가 admission 앞에서 제한된다. Admission 검사 배치와 정상 경로의 비용은 설계 전이므로 수치·속도 향상을 판정하지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_message_send_api.cpp:332-353,427-462`; `core/src/api/socket/part_helper_api.cpp:86-122,618-649`; `core/src/runtime/sockets/common/socket_base_msg.cpp:306-322`; `core/src/runtime/core/pipe.cpp:3704-3778`.
- 확신: 중간 — public MORE의 staging 경로는 확인됐지만 “incremental multipart”에 public staging까지 포함하는지는 두 문단의 계약 해석 결정이 필요하다(BLOCKERS).

### F-R1-14 Payload 없는 callback queue의 잔존 서술

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/08-runtime-boundary.ko.md:183-186`; `core/doc/spec/core/08-runtime-boundary.en.md:201-204`; 현재 public completion 소유권: `core/doc/spec/core/socket/README.ko.md:1123-1127,1160-1165`; `core/doc/spec/core/socket/README.en.md:1239-1243,1280-1289`; `core/doc/spec/core/socket/06-dealer.ko.md:44-47,62`; `core/doc/spec/core/socket/06-dealer.en.md:46-49,64`.
- 현재 규칙(인용): “각 lane의 payload는 directional network pipe에만 보관한다.” / “남은 terminal callback metadata queue에는 payload가 없는 timeout·disconnect·shutdown 결과 metadata만 둔다.”
- 문제: Runtime 내부 동작 절은 이전 callback 모델을 설명하지만 public 계약과 실제 구현은 reply payload를 가진 REQUEST completion을 socket-local queue에 넣는다. Queue reservation에 reply array pointer와 part count가 저장되고 completion receive에서 소유권이 caller로 이동한다. 여기서 “payload를 복사하지 않는다”는 문장만으로는 drift가 아니다. 문제는 보관 위치를 pipe로 한정하고 queue를 payload 없는 callback metadata로 한정한 서술이다.
- 제안: 소유자는 `socket/README`의 completion ownership이며 runtime-boundary에는 해당 계약 참조를 둔다: **“REQUEST reply payload는 socket-local completion record가 수신 또는 폐기까지 소유하고, successful completion receive가 그 소유권을 caller에게 이전한다.”**
- 규칙 수: before 2 → after 1 — callback metadata-only 모델과 payload 소유 completion 모델을 후자로 합친다.
- 행동 변경: 없음 — §5의 내부 구현 서술을 현재 public pull 계약에 맞추며 queue 구현은 그대로다.
- 영향: core — `core/src/api/socket/socket_completion_queue_internal.cpp:430-441`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — payload copy를 추가하거나 제거하는 제안이 아니다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_request_reply_internal.cpp:444-471`; `core/src/api/socket/socket_completion_queue_internal.cpp:430-441,475-491`.
- 확신: 높음 — queue가 payload 배열의 소유권을 보유·이전하는 코드가 직접 확인된다.

### F-R1-15 Option 계약의 discovery handle

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/socket/README.ko.md:393-394,646,651,673`; `core/doc/spec/core/socket/README.en.md:410-411,696,701,724-725`; Core 외부 책임: `core/doc/spec/core/08-runtime-boundary.ko.md:61-76,143-152`; `core/doc/spec/core/08-runtime-boundary.en.md:62-77,155-167`.
- 현재 규칙(인용): “`handle_`은 raw socket 또는 discovery다.”
- 문제: 공통 옵션의 적용 handle과 set/get 함수 설명에 이전 Core discovery 표면이 남았다. Runtime boundary는 discovery를 Framework 책임으로 제외하며 현재 option resolver도 socket만 해석한다. 해당 문장만 보면 application은 Core가 제공하지 않는 discovery handle을 생성해 raw option을 설정할 수 있다고 읽게 된다.
- 제안: 소유자는 `socket/README`의 common option 적용 대상 문장이다: **“Core의 공통 option set/get은 지원되는 raw socket handle에 적용하며, discovery 구성은 Core 외부의 해당 소유 계층 계약을 따른다.”**
- 규칙 수: before 2 → after 1 — raw socket·discovery 이중 handle 모델을 현재 raw socket 경계 하나로 통합한다.
- 행동 변경: 없음 — 현재 존재하는 public discovery handle/API를 삭제하는 제안이 아니라 이미 제거된 표면의 잔존 설명을 정리한다.
- 영향: core — `core/src/api/core/zlink_option.cpp:85-97`; binding·Framework의 discovery 구현은 미확인.
- 성능 영향: 없음 — option resolver나 data hot path를 변경하지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/api/core/zlink_option.cpp:85-97,100-156`; `core/src/api/socket/socket_api_internal.hpp:96-119`.
- 확신: 높음 — Core 경계 계약과 set/get의 실제 target 분기가 일치한다.

### F-R1-16 Monitor 검증 절의 lock 요구

- 분류: form
- 위치: `core/doc/spec/core/06-monitoring.ko.md:177-180,475-479,558`; `core/doc/spec/core/06-monitoring.en.md:187-190,495-498,600-603`. 작성 기준: `doc/principal/documentation/spec-writing-guide.ko.md:367-417,701-754`.
- 현재 규칙(인용): “pipe 합계 field 군은 하나의 lock 안에서 읽어 군 내부에서 일관되며”.
- 문제: 같은 lock 요구가 상세 설명, API 설명, 공개 검증 절에 반복된다. Public snapshot은 값의 일관성 범위를 관찰하게 하지만 lock 개수나 lock 획득 여부를 노출하지 않는다. §8의 이 항목을 그대로 test로 옮기면 내부 동기화 구현을 검사하게 되어 §9.3에 어긋난다. Cross-field 일관성 보장 자체를 삭제하자는 지적은 아니다.
- 제안: Lock 기구의 소유자는 `06-monitoring` §6.3의 내부 설명 한 곳으로 제한하고 §8은 관측으로 쓴다: **“Monitor snapshot은 pipe 합계 field 군 내부의 일관성을 제공하며 서로 다른 field 군 사이의 같은 시점 관측은 보장하지 않는다.”**
- 규칙 수: before 3 → after 1 — 같은 lock 기구를 규정하는 세 위치를 내부 설명 한 곳으로 줄인다.
- 행동 변경: 없음 — 공개 snapshot의 일관성 계약과 현재 locking은 유지한다.
- 영향: core — `core/src/runtime/sockets/common/socket_base_monitor.cpp:43-78`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — lock 제거 제안이 아니며 public 검증에서 lock 구조를 요구하지 않도록 정리한다.
- 근거 코드: C ABI/Core C++: `core/src/runtime/sockets/common/socket_base_monitor.cpp:43-78`; `core/src/api/monitoring/monitor_socket_api.cpp:28-58`(공개 monitor와 내부 socket의 경계).
- 확신: 높음 — 검증 문장에 내부 기구가 직접 포함되어 있다. 실제 snapshot 값의 동시성 테스트를 실행한 것은 아니다.

### F-R1-17 Endpoint 해제 검증의 transport 범위

- 분류: form
- 위치: `core/doc/spec/core/socket/README.ko.md:623-624,853-855,1327-1328`; `core/doc/spec/core/socket/README.en.md:673-674,910-912,1509-1511`. 결정 근거: `doc/plan/c016-worklog/decisions.ko.md:1343-1345`(D-098).
- 현재 규칙(인용): 본문의 “transport와 관계없이” / 검증 절의 “tcp·ipc·inproc 모두 성공하고”.
- 문제: Endpoint release-before-return 규칙은 transport 전체에 적용되지만 검증 요구는 tcp·ipc·inproc만 열거한다. WS·TLS 계열 listener도 같은 동기 release 명령을 구현하므로, 이들은 본문에 있는 공개 반환 경계의 검증 대상이다. 현재 test가 없다고 단정한 것은 아니다. R1에서 발견한 것은 스펙 검증 요구의 범위 누락이다.
- 제안: 소유자는 `socket/README`의 endpoint 해제 계약이고 §8도 같은 적용 범위를 참조한다: **“지원하는 각 transport에서 unbind 또는 bind socket의 close가 반환한 직후 같은 endpoint의 bind가 성공하며 후속 연결은 새 listener에만 도달한다.”**
- 규칙 수: before 2 → after 1 — 본문 전체 transport와 검증의 세 transport 범위를 하나로 맞춘다.
- 행동 변경: 없음 — D-098의 기존 계약을 검증 범위에 반영한다.
- 영향: core — `core/src/runtime/core/object.cpp:485-497`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — 기존 endpoint 해제 순서나 fd 처리 경로를 바꾸지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/runtime/sockets/common/socket_base_api.cpp:177-184`; `core/src/runtime/sockets/common/socket_base_endpoint.cpp:1099-1103`; `core/src/runtime/core/object.cpp:485-497`; `core/src/runtime/transports/ws/asio_ws_listener.cpp:339-346`; `core/src/runtime/transports/tls/asio_tls_listener.cpp:314-321`.
- 확신: 높음 — 본문과 검증의 적용 범위 차이가 명시적이다. 실제 transport별 bind 재현은 실행하지 않았다.

### F-R1-18 DISCONNECTED의 수신 독립성 검증 누락

- 분류: form
- 위치: `core/doc/spec/core/06-monitoring.ko.md:85,523,539-543`; `core/doc/spec/core/06-monitoring.en.md:93,551-553,573-578`. R1 전체 본문·검증에서 D-092의 “application recv 없이 peer termination에서 게시” 조건은 발견하지 못했다. 결정 근거: `doc/plan/c016-worklog/decisions.ko.md:1247-1252`(D-092).
- 현재 규칙(인용): “이미 성립한 연결의 종료는 transport와 무관하게 `DISCONNECTED` 하나로 보고하며”.
- 문제: 검증 절은 established 연결의 event 종류와 중복 CLOSED 부재를 규정하지만, D-092가 없앤 application drain 의존성을 관측하지 않는다. ROUTER에 수신 중 RID preamble이나 미수신 record를 남겨두는 조건이 없으면, 마지막 pipe release까지 DISCONNECTED를 늦추는 이전 구현도 이 문장을 만족한다. 현재 코드는 drain_complete를 사용하지 않고 peer termination에서 공유 claim으로 event를 한 번 게시한다.
- 제안: 소유자는 `06-monitoring`의 established connection 종료 계약과 그 §8 관측 항목이다: **“성립한 physical connection의 peer 종료는 application DATA receive가 진행되지 않고 수신 record가 남아 있어도 monitor에서 DISCONNECTED로 한 번 관찰된다.”**
- 규칙 수: before 1 → after 1 — D-092의 단일 종료 규칙을 유지하며 검증에 수신 독립 조건을 드러낸다.
- 행동 변경: 없음 — 현재 peer termination 게시 시점을 유지한다.
- 영향: core — `core/src/runtime/sockets/common/socket_base_api.cpp:1729-1784`; binding·Framework의 monitor 소비 동작은 언어별 미확인.
- 성능 영향: 없음 — poller·pump·timer를 추가하지 않으며 기존 event 게시 경로를 유지한다.
- 근거 코드: C ABI/Core C++: `core/src/runtime/core/pipe.cpp:2979-2984`; `core/src/runtime/sockets/common/socket_base_api.cpp:1729-1734,1763-1784`.
- 확신: 높음 — D-092의 원인과 현재 구현은 대응하지만 R1 검증 요구에는 그 구분 조건이 없다.

### F-R1-19 STREAM 영문 검증 항목의 결합

- 분류: form
- 위치: `core/doc/spec/core/socket/08-stream.en.md:513-516`; 대응 국문 `core/doc/spec/core/socket/08-stream.ko.md:475-479`. 원래 readiness 규칙: `core/doc/spec/core/socket/README.ko.md:1171-1174`; `core/doc/spec/core/socket/README.en.md:1298-1302`.
- 현재 규칙(인용): “reconnect.- `ZLINK_POLLCOMPLETION` is non-consuming level readiness.”
- 문제: 영문에서 RID snapshot 보존 검증과 completion readiness 검증 사이의 개행이 사라져 Markdown상 한 bullet이 됐다. 국문은 두 항목이다. 이 상태는 “각 항목은 test 하나”라는 대응을 깨고 영문 문서에서 readiness의 독립 관측을 숨긴다. Close 결과의 의미 문제는 F-R1-8이 소유하며 여기서는 편집 결합만 지적한다.
- 제안: 소유자는 `08-stream.en.md`의 Completion 검증 절이며 readiness를 별도 bullet로 복구한다: **“`ZLINK_POLLCOMPLETION` is non-consuming level readiness, and draining with `zlink_completion_recv(DONTWAIT)` through `NO_DATA` clears it.”**
- 규칙 수: before 2 → after 2 — 동작 규칙 추가 없이 국문과 같은 독립 검증 항목 경계를 복구한다.
- 행동 변경: 없음 — 영문 형식 수정만 필요하다.
- 영향: core — `core/src/api/socket/socket_completion_queue_internal.cpp:475-489`; binding·Framework 언어별 구현 미확인.
- 성능 영향: 없음 — 코드 변경이 없다.
- 근거 코드: C ABI/Core C++: `core/src/api/socket/socket_completion_queue_internal.cpp:475-489`; `core/src/api/socket/socket_completion_queue_internal.cpp:500-511`.
- 확신: 높음 — 국문·영문 bullet 경계 차이가 직접 확인된다.

### F-R1-20 저장·조회만 하는 pending 옵션

- 분류: complexity
- 위치: `core/doc/spec/core/socket/README.ko.md:388-402,1292-1294`; `core/doc/spec/core/socket/README.en.md:405-423,1445-1449`.
- 현재 규칙(인용): “ABI 보존 전용 (uint64_t, 기본 0; 저장·반환만 하고 동작에 영향 없음)”.
- 문제: `PENDING_MAX_MSGS`와 `PENDING_MAX_BYTES`는 제출 제한을 설정하지 않지만 두 식별자, 값 storage, default, get/set 규칙과 지원 socket type 규칙을 유지한다. `core/src`의 두 backing field 참조는 선언·초기화·setter·getter에만 있다. 이는 internal mode를 application이 선택해야 하는 표면도 아니며, 현재 ABI 호환을 위해서만 남은 상태다. 다만 set/get round trip 자체는 관찰 가능하므로 삭제를 행동 불변 정리로 취급할 수 없다.
- 제안: **삭제** — 0.18.0에서 ABI 보존 의무를 끝낼 수 있다고 감독이 결정한 경우 `socket/README` §5의 두 pending 옵션과 대응 공개 식별자·저장 상태를 함께 제거한다; 통합 문장 초안은 **“Admission 이전 payload를 제한하는 pending option은 제공하지 않는다.”**
- 규칙 수: before 2 → after 0 — 두 개의 무동작 설정·조회 계약을 제거한다.
- 행동 변경: 있음 — 기존 source의 옵션 참조와 set/get 결과가 달라지므로 현재 릴리스에서 삭제하면 안 된다.
- 영향: core — `core/src/runtime/core/options_core_socket.cpp:121-137,292-307`; binding의 공개 option enum과 호환 정책은 언어별 미확인.
- 성능 영향: 없음 — 두 값은 DATA·REQUEST admission hot path에서 읽히지 않는다. Option 상태·설정 분기 제거를 처리량 향상으로 계산하지 않는다.
- 근거 코드: C ABI/Core C++: `core/src/api/core/zlink_option.cpp:31-40,115-121,142-147`; `core/src/runtime/core/options_core_socket.cpp:121-137,292-307`; `core/src/runtime/core/options.cpp:119-120`; `core/src/runtime/core/options.hpp:237-238`.
- 확신: 높음 — source 전체 symbol 검색에서 admission 소비자가 없고 스펙도 무동작을 명시한다. 삭제 가능 버전은 감독의 ABI 결정 사항이다.

## 추가 후보(요약 1줄)

- `form`, 행동 변경 없음, 검증의 내부 기구 요구 2 → 0: `core/doc/spec/core/06-monitoring.ko.md:120-128,561-562` / `core/doc/spec/core/06-monitoring.en.md:130-137,606-610`의 private monitor PAIR SNDHWM·RCVHWM 및 worker admission을 공개 검증으로 재규정한 항목은 §5가 소유하고, §8 초안은 “설정한 byte 예산에 따른 monitor 수신·overflow 결과를 공개 API로 관찰한다”; 근거 C ABI/Core C++ `core/src/api/monitoring/monitor_socket_api.cpp:34-58`, `core/src/runtime/sockets/common/socket_monitor_runtime.cpp:242-262`, 다른 언어 미확인, 구체적인 overflow 관측은 §5의 기존 drop 정책을 그대로 사용해야 한다.
- `consolidation`, 행동 변경 없음, 초기화 전제 2 → 1: `core/doc/spec/core/02-message.ko.md:41-44,150-159,169-170,296-306` / `core/doc/spec/core/02-message.en.md:42-45,152-161,307-318`의 “모든 message 함수 전 초기화”와 adopt의 미초기화 destination 요구는 §2 소유 문장 “초기화·adopt의 destination을 제외한 message 입력은 초기화된 객체여야 한다”로 범위를 합칠 수 있다; 근거 C ABI/Core C++ `core/src/api/message/message_api.cpp:42-80,117-132`, 다른 언어 미확인.

## 읽은 범위

### 지정 스펙 전체

아래 각 행은 국문·영문 두 파일의 처음부터 마지막 행까지 읽은 결과다. 파일명 앞의 경로는 `core/doc/spec/core/`이며 `.ko.md`와 `.en.md`를 각각 읽었다. 생략한 지정 스펙은 없다.

| 파일 쌍 | 국문 실제 열람 행 수 | 영문 실제 열람 행 수 |
|---|---:|---:|
| `README.{ko,en}.md` | 101 | 95 |
| `00-public-contract-governance.{ko,en}.md` | 86 | 92 |
| `01-context.{ko,en}.md` | 373 | 396 |
| `02-message.{ko,en}.md` | 479 | 514 |
| `03-errors.{ko,en}.md` | 582 | 631 |
| `04-events.{ko,en}.md` | 120 | 138 |
| `05-polling.{ko,en}.md` | 327 | 345 |
| `06-monitoring.{ko,en}.md` | 567 | 617 |
| `07-utilities.{ko,en}.md` | 506 | 579 |
| `08-runtime-boundary.{ko,en}.md` | 275 | 316 |
| `glossary.{ko,en}.md` | 63 | 69 |
| `socket/README.{ko,en}.md` | 1,418 | 1,628 |
| `socket/01-pair.{ko,en}.md` | 220 | 214 |
| `socket/02-pub.{ko,en}.md` | 265 | 289 |
| `socket/03-sub.{ko,en}.md` | 296 | 324 |
| `socket/04-xpub.{ko,en}.md` | 287 | 267 |
| `socket/05-xsub.{ko,en}.md` | 286 | 351 |
| `socket/06-dealer.{ko,en}.md` | 479 | 516 |
| `socket/07-router.{ko,en}.md` | 501 | 524 |
| `socket/08-stream.{ko,en}.md` | 490 | 530 |
| **합계** | **7,721** | **8,435** |

### 기준 문서와 교차 범위

- `AGENTS.md`: 137행 전체. `doc/AGENTS.md`: 51행 전체. 작업 시작 branch는 `main`이었다.
- `doc/plan/c016-worklog/spec-review/README.ko.md`: 66행 전체.
- `doc/principal/documentation/documentation-principles.ko.md`: 474행 전체. 출력이 잘린 중간 구간은 별도 열람했다.
- `doc/principal/documentation/spec-writing-guide.ko.md`: 필수 판단 구간 `13-36,106-128,357-417,701-754`의 162행을 확인했다. 전체 파일 일괄 출력 중 잘린 다른 부분까지 전수 열람으로 계산하지 않았다.
- `doc/plan/c016-worklog/decisions.ko.md`: D-090·091 `1205-1211,1235-1241`, D-092 `1247-1252`, D-094 `1281-1286`, D-095 `1287-1292`, D-096·097 `1302-1315`, D-098~101 `1343-1378`, D-B119·120 `1277-1280,1294-1298`의 결정 구간을 포함해 관련 부분을 읽었다. 이 열거 구간은 91행이며 함께 출력된 다른 B 작업 기록은 계약 근거로 사용하지 않았다.
- D-096 대조를 위해 R2 범위인 `core/doc/spec/core/protocol/01-zmp.ko.md:179-210` 32행과 `core/doc/spec/core/protocol/01-zmp.en.md:186-216` 31행을 추가로 읽었다. 국문 `192-206` / 영문 `198-212`의 duplicate lane 전체 종료·wire에서 동시 attempt 구별 불가 설명은 `core/src/runtime/sockets/common/socket_base_api.cpp:343-351,442-468`과 대응한다. R1 범위에서는 D-096에 반하는 무조건 수렴 시간 보장을 발견하지 못했다. Protocol 전체의 판정은 R2에 남긴다.

### 구현 근거 열람

Core 구현은 public 함수부터 관련 호출 경로를 좁혀 읽었으며 `core/src/**` 전체를 전수 열람한 것이 아니다. 아래는 보고서 근거로 사용한 실제 열람 구간과 중복을 제외한 행 수다. 주변 탐색·symbol 검색 출력은 별도 행 수에 더하지 않았다.

| 구현 파일 (`core/src/` 기준) | 근거로 읽은 구간 | 행 수 |
|---|---|---:|
| `api/core/zlink_option.cpp` | 31-40,75-170 | 106 |
| `api/message/message_api.cpp` | 42-80,87-135 | 88 |
| `api/message/recv_result_internal.hpp` | 1-50 | 50 |
| `api/socket/socket_api_internal.hpp` | 75-145 | 71 |
| `api/socket/socket_message_api.cpp` | 119-151,383-387,430-451,533-552 | 80 |
| `api/socket/socket_message_recv_api.cpp` | 280-420 | 141 |
| `api/socket/socket_message_send_api.cpp` | 300-353,383-470,509-529,714-767 | 217 |
| `api/socket/part_helper_api.cpp` | 70-176,618-652,680-684,775-810 | 183 |
| `api/socket/request_reply_protocol_internal.hpp` | 105-135 | 31 |
| `api/socket/socket_request_reply_submit_api.cpp` | 102-129 | 28 |
| `api/socket/socket_request_reply_pending_api.cpp` | 308-329 | 22 |
| `api/socket/socket_request_reply_dispatch.cpp` | 397-421,449-500 | 77 |
| `api/socket/socket_request_reply_internal.cpp` | 444-489 | 46 |
| `api/socket/socket_completion_queue_internal.cpp` | 98-131,231-266,375-412,426-572 | 255 |
| `api/monitoring/monitor_socket_api.cpp` | 28-64 | 37 |
| `runtime/sockets/common/socket_base_api.cpp` | 164-192,343-351,442-468,1729-1790 | 127 |
| `runtime/sockets/common/socket_base_dispatch.cpp` | 350-378 | 29 |
| `runtime/sockets/common/socket_base_endpoint.cpp` | 1091-1107 | 17 |
| `runtime/sockets/common/socket_base_msg.cpp` | 299-330 | 32 |
| `runtime/sockets/common/socket_base_monitor.cpp` | 40-80 | 41 |
| `runtime/sockets/common/socket_endpoint_runtime.cpp` | 48-74 | 27 |
| `runtime/sockets/common/socket_public_handle.cpp` | 1-95 | 95 |
| `runtime/sockets/common/socket_send_complete.cpp` | 180-231 | 52 |
| `runtime/sockets/common/socket_monitor_runtime.cpp` | 242-262 | 21 |
| `runtime/sockets/router/router.cpp` | 26-41,76-96 | 37 |
| `runtime/sockets/router/router_admission.cpp` | 295-455 | 161 |
| `runtime/core/pipe.cpp` | 1606-1611,1625-1658,2972-2987,3704-3778 | 131 |
| `runtime/core/object.cpp` | 480-502 | 23 |
| `runtime/core/options_core_socket.cpp` | 116-141,287-310 | 50 |
| `runtime/core/options.cpp` | 119-120 | 2 |
| `runtime/core/options.hpp` | 237-238 | 2 |
| `runtime/transports/ws/asio_ws_listener.cpp` | 331-348 | 18 |
| `runtime/transports/tls/asio_tls_listener.cpp` | 307-324 | 18 |

검증 절의 번호는 파일마다 다르므로 제목 번호가 8인 절만 찾지 않고, 각 파일의 마지막 「구현 및 contract test 검증 요구」 절을 모두 읽었다.

Binding·Framework 구현과 spec, `core/doc/spec/core/systems/**`, 위에 명시한 protocol 구간 외의 R2 문서는 job 분할에 따라 전수 검토하지 않았다. `core/doc/internals/**`는 public 계약의 근거로 사용하지 않았다. Test source 전체와 gate script를 대조하지 않았으므로 “실제 test가 없다” 또는 “gate가 실패한다”는 판정은 하지 않았다. 성능 측정 결과도 없다.

## BLOCKERS

- F-R1-11: Multipart receive owner 위반에서 보존할 공개 결과는 socket README의 `INVALID_STATE/EBUSY`인가, `03-errors`와 현재 구현의 `BUSY/EBUSY`인가? 어느 쪽을 현행 계약의 정정으로 인정할지와 0.18.0 변경으로 분리할 범위를 감독이 결정해야 한다.
- F-R1-13: HWM의 “최종 크기를 모르는 incremental multipart”에는 public MORE의 socket-local staging도 포함되는가? 포함한다면 현재 public MORE 경로의 코드 변경이고, 제외한다면 첫 MORE부터 bounded하다는 공개 검증 문장의 적용 대상을 새로 확정해야 한다.
- F-R1-20: 0.18.0에서도 `PENDING_MAX_MSGS/BYTES`의 저장·조회 ABI 보존을 계속해야 하는가? 유지 의무가 있으면 현재 두 옵션은 삭제할 수 없으며, 본 보고서의 complexity 후보로만 남는다.
