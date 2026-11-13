# #4143 정적 진단

## 최초 실패와 커밋 범위

주어진 이력에서 가장 이른 실패는 2026-11-03의 `2278cc19f4`이며, request test의 다른 상한(:624)과 send/route allocation test도 함께 실패했다. 11-04의 `999d67638f`는 통과했고, 그 다음 재실패가 11-05의 `6f10186f97`이다. 따라서 6f는 통과 뒤 재발한 첫 실패 commit이지만, 이 사실만으로 원인 변경을 그 SHA 하나에 귀속할 수는 없다. 허용된 범위에서 요청 경로를 건드린 선행 커밋은 `3f83745bed`(요청 제출 할당 절감)와 `910d1fc41f`(서비스 wire를 native message에 인코딩)이다. 첫 커밋은 task/request projection과 제출 bookkeeping을 바꿔 할당을 줄이는 목적이며, 두 번째는 wire encode·복사를 줄이는 목적이다. 둘 다 로컬 Actor 등록 함수 자체에서 MSVC만 추가 할당하도록 만든 변경이라는 정적 근거는 없다.

`999d67638f..6f10186f97`의 요청 경로 관련 커밋은 `3f83745bed`와 `910d1fc41f`이다. 같은 범위의 `7c63570270`은 테스트 allocation counter의 new/delete 쌍을 맞추는 수정으로, 측정 결과의 런타임 할당 수를 의도적으로 늘리는 변경이 아니다. `6f10186f97..dc3a609aa9`에는 `10c9ac2f6a`가 핵심이다. receiver 상태를 한 블록에 소유시키고 등록 reply를 C1로 전달하도록 바꾸어 request receiver의 여러 별도 상태 할당을 합쳤고, 대신 등록 sink와 callback/registry 저장소는 여전히 필요하다. `021296e218`은 요청 제출 bookkeeping과 registry 코드를 다듬어 일부 복사·할당을 줄이는 목적이다. 나머지 `af48cd4775` 등은 해당 로컬 Actor 경로와 무관하다.

그 뒤 `5a9cccb778`은 request 경로의 payload 복사와 codec/registry 할당을 줄이는 수정이다. `46aa3d816d`는 Node 요청 header encode 중복을 제거하여 Node 요청의 인코딩 할당을 줄이는 수정이다. `4a4b07ff3d`는 source-side send 작업을 줄이는 변경이고, `85835f03ad`는 C++ binding submit과 socket close 순서를 맞추는 변경이다. 후자의 세 변경은 로컬 Actor의 `register_local_completion` 측정 구간을 직접 바꾸지 않는다. `0005a4d6c0`까지 살핀 관련 경로 변경의 방향은 대부분 할당 제거이며, 이 테스트의 로컬 Actor 등록에 할당을 추가했다고 입증할 커밋은 찾지 못했다.

## 테스트가 세는 것과 기준값

현재 테스트의 `LocalActorRequestCompletionRegistrationDoesNotExceedBaselineHeapCalls` 측정 구간은 `receive_public`로 공개 reply와 receiver를 준비한 뒤 `register_local_completion`을 호출하는 구간이다. host 구성·시작과 `prepare_local_operation_async()`는 측정 전에 끝난다. 기준은 `EXPECT_LE(sample.total, 8u)`이며, 이 값은 정확한 API 계약보다 2026-11-07의 `10c9ac2f6a`가 가져온 회귀 상한이다. 그 커밋은 주석에 `5ecae5de8b`에서 같은 local Actor 경로가 8회 할당했다고 기록했다. 즉 8은 과거 관측값을 새 소유 구조의 상한으로 옮긴 값이고 Windows 결과를 포함해 고른 값이라는 근거는 없다.

Node assertion은 `NodeRequestUsesOnlyItsPreparedReceiverAboveTheTransport`에서 encode 기준, target 기준, prepared receiver 기준을 각각 별도로 세고, `call.async<payload_t>()` 구간의 실제 할당이 그 합과 같은지 확인한다. 이어 제출 횟수가 1인지 확인한다. 이 검사는 실제 call이 기준보다 추가로 만든 등록·callback·projection 할당을 검출한다. 해당 줄은 현재 파일에서 734–740행이다.

## Windows 할당 목록과 후보 지점

Windows stack은 주소만 기록되어 심볼이 없으므로 아래는 크기와 코드 경로에 따른 후보 귀속이며, 각 이벤트의 확정 귀속은 아니다. 등록 측정은 동기 함수만 실행한다. `receive_public`, `register_local_completion`, `register_prepared_local_operation`, `operation_registry_t::register_operation` 경로에는 `co_await`가 없다. 따라서 이 측정의 1688/2280/2192바이트 이벤트를 coroutine frame이라고 단정하는 것은 맞지 않는다. 앞서 준비한 `prepare_local_operation_async`의 frame도 측정 scope 밖이다.

1688바이트(11-06)와 2280바이트(11-08, 11-09)는 `request_task_access_t::receive_public`의 `make_shared<receiver_state_t<...>>` 소유 블록 후보이다. 이 블록에는 admitted/reply/physical stage와 projection 저장소가 함께 들어간다. 크기 차이는 컴파일러·표준 라이브러리 ABI에 따른 embedded 타입 크기 차이나 해당 시점의 코드 형태로 설명할 수 있지만, 로그만으로 분리할 수 없다.

2192바이트는 `register_local_completion`의 `make_shared<operation_completion_t>()` 후보이다. 이 객체는 `receive_record_t`와 parts vector를 포함한다. 240바이트는 같은 함수에서 생성하는 `local_completion_sink_t` 소유 블록 후보이다. 88바이트(최초 2개 이벤트 중 하나)는 receiver callback/shared state 보조 블록 후보이다. 이벤트 순서상 `receive_public`가 먼저이므로 첫 큰 블록과 뒤따르는 작은 블록은 receiver 생성 쪽 후보로 볼 수 있다.

나머지 312/328, 152, 64, 128, 64, 88바이트는 `completion_sink_t`의 callable 저장, operation registry의 등록 map/deadline index 노드 또는 관련 allocator 노드 후보이다. 이 중 하나 이상은 `std::function` 계열 callable이 MSVC의 small-buffer 저장소를 넘어서 heap을 쓰는 비용일 수 있다. 각 크기는 C++ ABI와 allocator 요청 크기라 STL 내부 타입이나 콜사이트를 단독으로 식별하지 못한다. 특히 312에서 328로 바뀐 것도 같은 논리 객체의 크기 차이라고 확정할 증거는 없다.

Node request 로그의 `2064`는 `receive_public` receiver block 후보이며, 같은 시기 기준 receiver 로그에도 `2064,104`가 나타난다. 11-06의 추가 `1552`는 알려진 peer가 즉시 submit되지 않아 대기 경로로 들어간 경우 `submit_registered_request` coroutine frame 후보이다. 그 함수는 `run_request_admission(...)` task를 `make_shared<task_t<void>>`에 보관하므로 frame/task가 caller 밖으로 탈출한다. 이 소스만으로는 GCC가 해당 프레임을 HALO로 제거한다고 결론낼 수 없고, 특히 탈출하는 frame은 표준의 중첩 수명 elision 조건을 충족한다고 보기 어렵다. 이 1552바이트를 로컬 Actor 등록의 실패 원인과 섞어서는 안 된다. 96바이트 이벤트들은 envelope/message/callback 또는 registry 노드의 후보이고, 640바이트 이벤트는 Node request submit closure/owner 저장소의 후보이다. 11-05에는 실제 11 대 기준 10이었고, 11-06에는 실제 6 대 기준 5였다. 11-08/09에는 기준 로그와 실제 구성 자체가 달라져 실제 9 대 기대 4 또는 5로 벌어졌다. 이는 줄 번호만 같은 단일 고정 allocation-site 실패로 묶을 수 없음을 보여준다.

## Linux와 Windows 차이 및 판정

이 실패군은 Linux에서 통과하고 Windows MSVC Release에서만 나타났으며, 11-03의 이전 실패(:624, 실제 22 대 기대 `<22`)와 send/route allocation 상한 동반 실패, 11-04 통과, 11-05 이후 재발 순서는 Windows allocation-count 기대치의 반복적 불안정을 보여준다. 따라서 공통 원인은 요청 의미의 런타임 결함이라기보다 Linux에서 얻은 allocation count 상한을 MSVC의 표준 라이브러리 저장·할당 동작에 그대로 적용한 데 있을 가능성이 높다. 다만 MSVC에서 실제 heap 할당이 더 발생하면 그 호출 비용은 Windows 실행의 실제 비용이다. 이 점에서 “측정 오류”라고 할 수는 없다. 현재 자료는 제품 코드의 불필요한 중복 할당이라고 확정할 근거를 주지 않으며, 기대값이 Linux에 치우친다는 판정이 더 잘 맞는다.

이번 local Actor 경로에 동일 receiver를 두 번 새로 만든 중복 지점은 확인하지 못했다. `receive_public`가 receiver state를 만들고, 등록은 그 receiver를 sink로 옮겨 보유한다. `operation_completion_t`와 sink는 결과 상태와 완료 실행자를 각각 소유하므로 같은 객체를 중복 구축하는 것은 아니다. request path의 근본적인 중복이 아닌 STL callable/registry 할당인지 확정하려면 symbolized stack이 필요하다. 가능한 제품 개선은 심볼이 확인된 과대 callable을 캡처 축소하거나 소유 블록에 합치는 것이며, 지금 특정 줄을 고치는 것은 근거가 부족하다. 테스트는 Windows 관측값을 반영해 플랫폼별 상한을 두거나, 호출 지점별로 측정 대상을 나누는 편이 타당하다.

## 심볼을 얻는 CI 변경

다음 win-x64 Framework C++ nightly에서 allocation trace를 만드는 테스트 executable의 PDB를 빌드 산출물로 보존하고, `describe_allocations`가 각 `CaptureStackBackTrace` 주소를 기록할 때 동일 executable PDB를 `DbgHelp`의 `SymInitialize`/`SymFromAddr`로 해석해 함수명과 소스 파일·줄을 출력하도록 한다. PDB와 executable의 동일 빌드 ID를 보장하고 심볼 경로를 CI job에 설정해야 한다. 수정·디스패치는 하지 않았다. 비교 대상은 11-09의 `0005a4d6c0` 재실행보다, 현재 실패가 있는 `r4137`와 같은 테스트 바이너리/Release 설정으로 수행하는 다음 win-x64 run이다. 로그를 비교하면 상한 초과 두 이벤트가 receiver, callable, registry 중 어디서 나오는지 결정할 수 있다.
