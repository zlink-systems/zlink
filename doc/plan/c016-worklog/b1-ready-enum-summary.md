# READY protocol error 구현 요약

## 결과

- `ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY = 0x10000016`을 Core public enum과 C/C++/Go/Rust raw header mirror, Python `ProtocolError`에 반영했다.
- paired READY frame 검증이 실패하면 `HANDSHAKE_FAILED_PROTOCOL(value=0x10000016)`을 먼저 기록한 뒤 기존 `protocol_error` 종료 경로가 `DISCONNECTED`를 기록한다.
- 두 connection을 비교해야 드러나는 count-2 duplicate lane과 socket type/Routing-Id 충돌은 기존 socket admission reject가 판정한다. 이 지점에서 종료할 각 network physical connection의 READY protocol event를 먼저 기록한다.
- count-2 lane set이 `HANDSHAKE_IVL` 안에 완성되지 않으면 generic timeout이 아니라 READY protocol error로 끝낸다. 근거는 `core/doc/spec/core/protocol/01-zmp.ko.md:190-193`의 “count 2에서 lane이 중복되거나 HANDSHAKE_IVL 안에 모두 오지 않은 경우” 조항이다. Count-1과 non-paired timeout 동작은 유지했다.
- 기존 monitor test에는 HELLO protocol error의 numeric value를 검사하는 case가 없었다. HELLO 상수 `0x10000013`과 기존 event 발생 경로는 바꾸지 않았다.

## 변경 파일

Repository 구현·test 변경:

- `core/include/zlink_enum.h`
- `bindings/c/include/zlink_enum.h`
- `bindings/cpp/include/zlink_enum.h`
- `bindings/go/include/zlink_enum.h`
- `bindings/rust/include/zlink_enum.h`
- `bindings/python/src/zlink/contracts/errors/codes.py`
- `core/src/runtime/engine/asio/asio_zmp_engine.cpp`
- `core/src/runtime/sockets/common/socket_base_api.cpp`
- `core/tests/integration/test_dealer_router_single_lane_contract.cpp`

작업 시작 전에 이미 변경되어 있었고 이 작업에서는 수정하지 않은 감독관 소유 파일:

- `core/doc/spec/core/06-monitoring.ko.md`
- `core/doc/spec/core/06-monitoring.en.md`

작업 기록:

- `/home/hep7/project/zlink-work/c016/b1-ready-enum-progress.md`
- `/home/hep7/project/zlink-work/c016/b1-ready-enum-summary.md`

`doc/**`, `core/doc/**`, `framework/**`에는 이 작업이 새 변경을 만들지 않았고 `scripts/local-package/**`는 실행하지 않았다.

## Engine 변경 지점

- `core/src/runtime/engine/asio/asio_zmp_engine.cpp:387-415`: count-2 pair fence timeout을 READY incomplete protocol error로 분류하고 READY enum value의 event를 낸 뒤 `protocol_error`로 종료한다.
- `core/src/runtime/engine/asio/asio_zmp_engine.cpp:754-766`: paired READY command의 metadata/count/lane/socket type/Routing-Id/initiator lane/pair identity 검증 실패를 한 지점에서 READY protocol event로 기록한다.
- `core/src/runtime/sockets/common/socket_base_api.cpp:437-465`: 두 physical connection의 admission 비교에서 판정하는 duplicate lane 및 cross-lane topology/identity 충돌에 대해 각 종료 connection의 protocol event를 먼저 기록한다.
- `core/tests/integration/test_dealer_router_single_lane_contract.cpp:1442-1609`: 누락·길이·값·count 불일치·count-1 lane-1·duplicate·count-2 missing lane에서 value와 동일 connection의 event 순서를 검증한다.
- `core/tests/integration/test_dealer_router_single_lane_contract.cpp:1613-1706`: old READY의 Lane-Count 누락 두 connection에서도 같은 계약을 검증한다.

## Gate

- `ulimit -v 16777216 && cmake --build core/build -j4`: 성공, 100% build.
- 보강 case 반복: `test_sl_wire_mandatory_lane_count_rejections` 3/3 통과.
- 보강 case 반복: `test_sl_wire_old_peer_without_lane_count_rejected` 3/3 통과.
- `ulimit -v 16777216 && ctest --test-dir core/build -j2 --output-on-failure`: 134/134 통과, 실패 0.
- `ulimit -v 16777216 && ZLINK_CORE_SOURCE=local bash bindings/python/tests/run_tests.sh`: 144 tests 통과, 4 subtests 통과, samples 7/7 통과.
- raw mirror `cmp`: 12/12 통과 (`zlink_enum.h`, `zlink/socket/api.h`, `zlink/eventing/api.h` × C/C++/Go/Rust).
- `git diff --check`: 통과, 오류 0.

## BLOCKERS

- 없음.
