[English](./framework-cpp-0.30.2.md) | [한국어](./framework-cpp-0.30.2.ko.md)

# ZLink C++ Framework 0.30.2 릴리스 노트

Framework 0.30.2은 C++ binding 1.20.0과 Core 1.20.0을 사용합니다.

## 변경

- request의 수락과 reply 완료를 구분해, 수락 후 응답을 기다리지 않고 후속 작업을 진행할 수 있게 했습니다. Join과 deferred 작업의 완료도 Actor FIFO 순서로 전달합니다. (#2069, #2040, #2119)
- Channel 수신의 received·admitted·dispatched 단계를 message flow에 기록합니다. (#2412)
- per-message topology·owner lease 조회와 completion 처리를 줄이고, ingress에서 header를 한 번 해석해 payload를 중간 복사 없이 전달합니다. (#1829, #2091)
- runtime metric(STREAM 연결, Actor population, Location lease, relocation 결과)을 스펙이 정한 관측 위치에서 기록합니다. (#2189)

## 결함 수정

- relocation과 Join 중 source·target의 authority, sealed inventory, completion 소유권이 엇갈려 Actor가 중복 존재하거나 종료·handoff가 멈추는 문제를 수정했습니다. (#1923, #2048, #2039, #2086, #2121, #2124, #2386, #2432)
- aggregate commit 중 이전 authority를 읽어 owner가 중복되던 문제를 수정했습니다. (#2423)
- Spot request의 늦은 terminal이 해제된 resolver에 전달되거나 relocation 직후 새 owner route가 준비되기 전에 요청이 실패하던 문제를 수정했습니다. (#2416, #2414)
- stream 연결 retirement와 dispatch callback 수명이 엇갈리던 문제를 수정했습니다. (#2302)
- Windows·macOS 빌드 및 Boost 기반 실행에서 발생하던 플랫폼별 결함과 테스트 환경 문제를 수정했습니다. (#2145, #2035, #2322)
- Bingo 관찰자가 늦게 Join해 이미 확정된 reward를 놓치던 sample 문제를 수정했습니다. 관찰 완료 뒤 현재 상태와 확정 reward를 조회합니다. (#2427)
- relocation 뒤 새 Entry Spot에 도착한 요청의 응답이 client에 전달되지 않던 문제를 수정했습니다. source 정리, sealed inventory, authority 인코딩을 각각 한 곳에서 결정하고, execution turn 안의 동기 대기를 없앴습니다. (#2109, #1788)
- Core 1.20.0은 handshake 중 completion poller를 등록할 때 임시 transport owner의 executor가 멈추던 문제를 수정했습니다. (#2504)
- Actor reply를 STREAM frame 한 개로 보내고 오류 payload를 스펙의 `{code, message}` 형식으로 맞췄습니다. 네 언어가 같은 공유 vector로 검증합니다. (#2507)
- relocation 뒤 같은 node로 돌아온 Actor에 Session을 bind하면 오래된 authority로 거부되던 문제, Message Follow로 돌아온 요청이 처리되지 않던 문제, relocation terminal reply가 원래 요청자 대신 coordinator로 전달되던 문제를 수정했습니다. (#2507)
- GameQuest sample이 Close를 확인한 뒤 다음 조회를 보내도록 수정했습니다. (#2507)

## 설치

[`framework-cpp/v0.30.2` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.2)에서 플랫폼별 Framework archive를 선택하거나 `bootstrap.cmake`로 내려받습니다. 서버 Framework에는 C++ binding 1.20.0과 Core 1.20.0이 필요합니다. Stream Connector만 사용하는 client에는 Core와 binding이 필요하지 않습니다.

릴리스 태그는 [`framework-cpp/v0.30.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.2)입니다.
