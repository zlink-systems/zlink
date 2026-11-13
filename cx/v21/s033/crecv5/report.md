# PR #3919 — local completion sink 계수 보고

## 변경

`public_host_runtime_t::register_local_completion`의 callback lambda를 익명 namespace의 `local_completion_sink_t` functor로 옮겼습니다. functor는 `source`, `result`, `completion`, `incomplete`, `receiver`를 소유하며 기존 `operator()`의 완료 처리는 유지합니다.

`std::make_shared<local_completion_sink_t>`가 성공한 다음 fault hook을 실행하고, 그 뒤 `std::exchange(operation.receiver, {})`로 receiver를 이동합니다. 따라서 sink 소유자 생성이 실패하면 호출자가 receiver를 계속 소유해 physical 실패를 전달합니다. C1 등록이 거부되면 이미 만들어진 `complete` sink가 receiver를 완료합니다. hook은 receiver 이동 직전에 유지했으며, `LocalCompletionSinkConstructionFailureDeliversPhysicalFailureOnce`가 포함된 `test_cpp_framework_actor_late_result`가 통과했습니다.

`completion_sink_t(std::shared_ptr<TOwner>, TInvoke)`를 사용했습니다. 이 생성자는 shared owner와 비캡처 기본 생성 invoker를 `request_callback_t`에 보관합니다. `request_callback_t`의 owner+empty-invoke 생성자는 shared pointer와 호출 함수 포인터만 설정하므로 callback을 위한 두 번째 heap 할당이 없습니다.

요청별 delivered flag나 engaged-optional 검사는 추가하지 않았습니다. `unknown_node_observer_failure`에서 crecv4가 추가한 미사용 `deliveries` 지역 변수도 제거했습니다.

## Heap calls per request

같은 allocation-event 계수 테스트로 `receive_public` 수신자 준비와 `register_local_completion` 구간을 측정했습니다. Host 시작과 operation reservation은 측정 전에 준비했습니다.

| 버전 | Heap calls | 기준 대비 | 계측된 allocation 크기 (bytes) |
|---|---:|---:|---|
| 5ecae5de8b | 8 | 기준 | 1488, 80, 1992, 144, 368, 144, 104, 40 |
| crecv4 receiver copy | 9 | +1 | 1488, 80, 1992, **80**, 144, 368, 144, 104, 40 |
| 현재 변경 | 8 | 0 | 1488, 80, 1992, 144, 368, 144, 104, 40 |

crecv4에서 추가된 80-byte 할당은 `operation.receiver` 복사에서 발생했습니다. 새 테스트 `LocalActorRequestCompletionRegistrationDoesNotExceedBaselineHeapCalls`는 현재 요청당 계수가 5ecae5de8b 기준인 8회를 넘지 않는지 검사합니다.

## Runtime ownership record

- Owner: C++ Framework host가 physical receiver를 local completion sink로 이전하고, operation registry의 pending entry가 terminal 승자를 정합니다.
- Spec: Submit/completion §9는 준비한 receiver가 physical 결과를 한 번 처리하도록 정하고(:331, :333), §10.1은 logical request마다 pending entry 하나가 terminal 권한을 소유하도록 정합니다(:447-449).
- Cross-language: .NET `ZLinkRequestAdmission.Run`, Java completion bridge, Node promise completion은 admission 또는 terminal 실패를 request 실패로 전달합니다. 이번 변경은 C++ 내부 callback의 소유 표현과 할당만 바꾸므로 다른 언어의 동작은 바꾸지 않았습니다.
- Classification: B, C++ physical receiver 소유권 결함을 고친 경로에서 생긴 receiver 복사 비용을 제거했습니다.
- Physical reply 전달 판정 위치: 2 → 2.

## 검증

- Full Release build: 통과, Ninja action 133/133, `-k 0`.
- Unit + contract CTest: 통과, 297/297, 231.18초. actor late-result와 request allocation 테스트를 포함합니다.
- 포커스 allocation test: 통과, 현재 요청당 8회.
- Format check와 `git diff --check`: 통과.
- 완료 시 `MemAvailable`: 약 26.5 GiB, 기준 12 GiB 이상.
- GCC는 `public_host_runtime.cpp` 컴파일 중 libstdc++ vector 비교에서 `-Wstringop-overread` 경고를 냈습니다. baseline 빌드에서도 같은 경고가 있었고 빌드 결과에는 영향을 주지 않았습니다.

실행 명령:

```sh
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh \
  cmake --build --preset linux-ninja-release --parallel 3 -- -k 0
flock -s /root/jobs/fx/cx/measure.lock \
  ctest --preset linux-ninja-release -L '^(unit|contract)$'
scripts/format/format.sh --check cpp
```

리팩토링 점검: 성능 1건, POSDDD 2건, 불필요 코드 1건 발견. 고친 항목: receiver 복사 제거, crecv4에서 수정한 physical receiver 소유권 결함 2건 보존, 미사용 `deliveries` 제거. 넘긴 항목: 없음.
