[English](./framework-cpp-0.31.0.md) | [한국어](./framework-cpp-0.31.0.ko.md)

# ZLink C++ Framework 0.31.0 릴리스 노트

Framework 0.31.0은 C++ binding 1.21.0과 Core 1.21.0을 사용합니다. (#3164)

## 변경 사항

- STREAM packet과 relay API에서 메시지 형식을 `framework::message_t`로 통일했습니다. (#2681, #2711)
- host가 application이 제공한 OpenTelemetry `MeterProvider` 하나로 모든 계기를 기록합니다. (#2874, #2887)
- DTO typed JSON 변환을 `dto_traits` 기반 공통 traversal로 통일하고 C++ wire 예시를 추가했습니다. (#1713, #1757, #2751, #2951)
- message-flow·dispatch-error telemetry를 process 전달 worker 하나에서 전달합니다. 호출 thread에서 logger·observer를 실행하지 않으며, 대기 8192건을 넘으면 drop 수를 `zlink.observability.events.overflow` metric으로 내보냅니다. (#2758, #3241)

## 결함 수정

- handler가 대기 중일 때 route worker를 반환하면서 executor credit으로 호출을 보유해 Host.stop이 handler의 물리적 종료를 기다립니다. (#2758, #3201)
- Session handler가 대기 중에도 STREAM 연결 해제 정리를 진행합니다. (#3195)
- Spot Close deadline이 Store 조회와 원격 작업 전반에 유지됩니다. (#3179, #3188)
- manual dispatch가 기존 owner mailbox를 통해 drain되고, ClientServer 수신·dispatch·reply의 FIFO 및 종료 처리를 수정했습니다. (#2758, #2877, #2885, #3183)
- route handler를 한 번 조회하고 handler 실행 직전에 permit을 취득하며, 수신 codec은 wire content-type을 따릅니다. (#2758, #2844)
- Spot 종료 순서와 callback 취소·drain을 정리해 종료 시 Core context와 callback이 늦게 해제되는 문제를 수정했습니다. (#2758, #2848, #2929, #2950)
- Join 실패 결과에 스펙의 실패 Kind와 Store membership Conflict 원인을 반영합니다. (#2738, #2913)
- Closing Spot의 Join·Close 결과와 relocated User Spot의 `GetOrCreate` authority 조회를 수정했습니다. (#2672, #2675, #2607, #2683)
- Channel send가 suspend 이후에도 호출 인자를 유효하게 보유하고, bound Session의 Actor 요청은 정확한 route를 사용합니다. (#2507, #2593, #2611)
- STREAM host의 `port "*"` 바인딩과 연결 해제·dispatch callback 수명을 수정했습니다. (#2295, #2302, #2618, #2619)
- Actor bind·relocation·deferred Join 처리와 늦은 terminal 전달의 오류를 수정했습니다. (#1786, #2109, #2119, #2128, #2227, #2261, #2385, #2406, #2507, #2544, #2573)
- Session·Spot lifecycle과 연결 소유권, liveness 정리, 요청 실패 분류 및 Actor reply wire 형식을 스펙에 맞췄습니다. (#1710, #2189, #2263, #2360, #2369, #2380, #2513, #2797)
- Actor terminal drop 진단과 `zlink.spot.count` label·값을 정리하고 drop 계기 소유자를 하나로 통합했습니다. (#2149, #2886, #3162, #3166, #3169)
- Worker 생성 실패 시 작업을 원자적으로 거부하고, Actor durable admission 및 공급 시점을 owner turn과 idle queue head에 맞췄습니다. (#3167, #3172, #3174)
- state lane infrastructure worker 공유, Session control 우선순위 및 ClientServer·Fanout FIFO/drain 동작을 수정했습니다. (#2758, #2791, #2885, #3136)
- 연결이 준비되기 전에 들어온 remote request를 admission owner 하나가 판정합니다. 준비된 연결에서는 대기 구독 없이 바로 제출합니다. (#1710, #3256)
- `GetOrCreate`가 새 생성 capacity를 보기 전에 기존 authority를 확인합니다. capacity가 모두 쓰였을 때 이미 있는 User Spot을 다시 얻는 요청이 실패하던 문제를 수정했습니다. (#2799, #3242)
- handler가 다른 작업을 기다릴 때 continuation 등록 할당이 실패하면 Host.stop이 handler 종료 전에 끝나던 문제를 수정했습니다. 등록 경로의 할당을 없앴습니다(2→0). `task.hpp`가 바뀌었으므로 application을 다시 빌드해야 합니다. (#2758, #3260)

## 성능 개선

- request transport, local enqueue, send producer·saturation, route submit 및 receive 비용을 줄였습니다. (#1672, #2579, #2616)
- state lane 완료 처리와 idle admission 경로를 개선했습니다. (#1672, #2610)
- 일반 claim dispatch의 coroutine frame 할당을 줄였습니다. 측정 결과는 12회·10.8 KB에서 7회·1.2 KB로, 31% 빨라졌습니다. (#1672, #2418)
- public node send의 lane closure와 전달 coroutine 비용을 제거했습니다. (#1672, #2377)
- Actor STREAM header를 최종 message buffer에 직접 쓰고 typed envelope header 복사를 없앴습니다. (#1672, #2258, #2364)
- idle host worker에 receive 역할을 넘겨 처리합니다. (#1672, #1867)

## 알려진 제한

- 시험 코드의 private 멤버 접근을 공개 경로로 옮기는 작업을 Node·Java·.NET에서 9차까지 진행했습니다. 남은 범위는 다음 릴리스에서 이어 갑니다. 제품 동작에는 영향이 없습니다. (#2309)
- Windows에서 mailbox signaler가 AF_UNIX datagram 실패 뒤 stream으로 전환하면 socket마다 임시 경로를 만들고 지우는 데 약 2.5ms가 듭니다. 수정은 다음 Core 릴리스에 포함됩니다. (#3053)

## 설치

[GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.31.0)에서 플랫폼별 Framework archive를 선택하거나 `bootstrap.cmake`로 내려받습니다. 서버 Framework에는 C++ binding 1.21.0과 Core 1.21.0이 필요합니다. Stream Connector만 사용하는 client에는 Core와 binding이 필요하지 않습니다.

릴리스 태그는 [`framework-cpp/v0.31.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.31.0)입니다.
