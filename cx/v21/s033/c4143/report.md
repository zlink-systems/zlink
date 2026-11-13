# #4143 — C++ node request 할당 경로

## 결과 (Linux 전/후)

| 표본 | 전 | 후 |
|---|---:|---:|
| Node encode 기준 | 1회, 160 B (eager `encode_payload_parts`) | 4회, 96+17+720+16 B (실제 deferred 경로) |
| Node target 기준 | 0회 | 0회 |
| Node receiver 기준 | 2회, 1,872+104 B | 2회, 1,872+104 B |
| Node 실제 request | 3회, 1,872+704+104 B | 6회, 1,872+96+17+720+16+104 B |
| Local Actor 등록 | 8회, 2,016+80+1,992+144+384+144+104+40 B | 동일 |

Node 기준 표본을 `create_envelope → metadata copy-assign → serialize → encode_service_payload_parts`로 맞췄다. 이전 eager 표본과 후속 표본은 경로가 달라 총계끼리 직접 비교할 수 없다. 후속 assertion은 `6 = 4 + 0 + 2`를 확인한다. Linux `std::map` 이동은 별도 allocation으로 나타나지 않는다.

Envelope allocation suite 전/후 주요 출력은 동일했다: typed Channel send `1,537/128 calls`, request `2,432/128`, escaped canonical header `6` 대 SAX fallback `15`, csite header views/container `0/0`. 할당 크기는 이 suite가 출력하지 않는다. 원시 로그: `/root/jobs/fx/cx/v21/s033/c4143/envelope-before.log`, `envelope-final.log`.

## 판정 위치

- Metadata: 값 전달과 move 대입 두 지점에서 header로 복사 대입 한 지점으로 축소 (`2→1`).
- Header: Windows attribution 기준 map 이동 다섯 지점에서 `prepare_service_payload`의 단일 state 생성 이동 한 지점으로 축소 (`5→1`).
- Deque: state lane과 offload executor의 cold 첫 enqueue 비용은 각각 양쪽에 남기는 기준(`1+1`); 제품 코드는 변경하지 않았다.

metadata 수명은 `channel_request_call_t::start()`가 `_metadata`를 참조로 동기 호출에 전달하고([call.hpp:195-209]), `submit_request_reply_message_erased`가 coroutine이 아닌 동기 함수로 첫 suspension 없이 `header.metadata = metadata`를 실행하는 흐름([channel_runtime.cpp:1729-1737, 1857-1861])에서 확인했다. 새 header는 즉시 자체 map snapshot을 소유한다.

## 바꾼 것

- `channel.hpp:1020-1027`, `channel_runtime.cpp:1729-1737, 1857-1861`: erased submit metadata를 `const map&`로 받고 header에 한 번 복사 대입.
- `channel_runtime.hpp:198-203`, `client_call_codec.hpp:43-51`, `client_call_codec.cpp:71-91`, `envelope_codec.hpp:258`, `envelope_codec.cpp:565-615`: 4인자 encode와 deferred prepare에 header rvalue reference를 전달. 하나의 `state_t`가 header/body/type/channel/size를 소유하며 header를 한 번 이동한다. 반환 lambda는 `shared_ptr` 하나(64-bit 대상에서 16 B)만 capture한다. 지연 encoder 실행도 이 소유 상태의 수명 안에 둔다.
- `test_cpp_framework_request_call_allocations.cpp:706-747`: Node 기준을 metadata 포함 실제 service encoder 경로에 맞추고 실제 request에도 같은 metadata를 설정.
- Local Actor 등록 기준 재구성은 중단했다. 기존 test access는 등록 통합 경로만 제공하며, receiver block·completion·비공개 local sink·registry 항목·두 cold deque enqueue를 각각 분리 측정할 접근점이 없다. 테스트 전용 API를 추가하지 않았다. 기존 `<=8` assertion은 남아 있다.

## 검증

- Release configure: 사용자 지정 Linux Ninja preset, Core 1.24.0 release prefix, binding source 옵션 사용 (`configure.log`).
- 전체 타깃 빌드: `build-final.log`, `612/612` 성공. 주입 제거 뒤 등록 테스트 타깃 재빌드 `build-cleanup.log`, `3/3` 성공.
- 요청 할당 테스트 53/53 통과 (`request-final.log`); envelope 할당 테스트 17/17 통과 (`envelope-final.log`).
- 전체 CTest: 315개 중 313개 통과, 2개 실패 (`ctest-final.log`). `test_cpp_framework_tooling_contract`는 smoke configure에서 `zlink_cppConfig.cmake`를 찾지 못했고, `test_cpp_framework_install_consumer`는 설치된 `zlink_framework_cppConfig.cmake`가 없다고 보고했다. 나머지 실패는 없다.
- 회귀 주입: Node 실제 경로에 임시 by-value header 복사를 추가했을 때 실제 `8` 대 기준 합 `6`으로 테스트 실패 (`regression-node.log`, assertion 줄 747). 등록 측정에 임시 `make_shared` 한 건을 추가했을 때 `9` 대 기존 상한 `8`로 실패 (`regression-registration.log`, assertion 줄 222). 두 임시 변경은 제거했고 깨끗한 테스트 타깃을 다시 빌드했다.
- 포맷: `bash scripts/format/format.sh --check cpp` 통과 (`format-final.log`).

## 런타임 판정

- 소유 계층·스펙: C++ Framework의 route request submit과 envelope codec이 header snapshot과 deferred encoder 소유 상태를 맡는다. E1 추가 복사 0건 원칙은 `08-messaging-hot-path.ko.md:96-100`, metadata snapshot 계약은 `02-channel-messaging.ko.md:617-690`.
- 교차 언어: public metadata 및 wire 동작을 바꾸지 않는 C++ 내부 비용 수정이다. .NET/Java/Kotlin의 계약·결정은 변경하지 않았고 언어별 동작 차이는 추가하지 않는다.
- 분류: B — 기존 불필요한 map 이동 및 `std::function` heap capture 비용 제거.

리팩토링 점검: 성능 2건, POSDDD 0건, 불필요 코드 0건 발견. map 소유 경계 2건을 수정했고, 등록 테스트의 분리 기준은 기존 접근점 부재로 넘겼다.

## 남은 문제

- Local Actor 등록 테스트는 요청된 합산식 재구성을 하지 못했고 기존 상한을 유지한다. 원인은 test-only API 없이 내부 컴포넌트별 비용을 분리할 수 없는 점이다.
- 전체 CTest의 tooling-contract와 install-consumer 두 검증은 위 패키지 설정 누락으로 실패했다.
