C++·.NET monitor poller parity 수정 완료

결과: PASS. BLOCKERS: 없음. 남은 테스트 실패: 없음.

변경
- C++ `bindings/cpp/src/Runtime/Eventing/poller.cpp:637,662`: monitor add/modify에서 POLLIN 밖의 모든 bit를 `config_error_t(config_result_t::invalid_argument, EINVAL)`로 거절한다. None(0)은 허용한다.
- C++ `bindings/cpp/include/zlink/Contracts/Eventing/poller.hpp`: monitor mask 오류 계약 주석을 추가했다. 기존 공개 시그니처는 변경하지 않았다.
- C++ `bindings/cpp/tests/contract/test_cpp_contract_monitor.cpp:236`: inproc/tcp에서 잘못된 add/modify, 등록 상태 보존, None→PollIn modify, READY/slot/source 검증, ready 상태 remove 후 미전달, 재등록 및 DONTWAIT drain을 검증한다. POLLOUT, POLLCOMPLETION, 각각 POLLIN과의 혼합, POLLERR, POLLPRI, 미지 bit를 검사한다.
- C++ `bindings/cpp/samples/run_samples.sh`: 샘플 빌드가 ZLINK_BUILD_JOBS를 따르게 했다(검증 시 3).
- .NET `bindings/dotnet/src/Zlink/Contracts/Eventing/Poller.cs`와 `Runtime/Eventing/Poller.cs:61,138,186`: IPoller에 monitor Add/Modify/Remove 오버로드를 추가했다. 기존 PollItem 목록과 native poller 등록 경로를 사용하며 monitor 참조를 등록 수명 동안 유지한다. completion 소유권은 획득하지 않는다.
- .NET `bindings/dotnet/src/Zlink/Runtime/Options/EnumValidation.cs:30`, `Runtime/Eventing/ZlinkPoll.Core.cs`, `Runtime/Sockets/SocketCallbacks.cs`: reusable/one-shot monitor poll 모두 같은 mask 검증을 적용한다. 기존 monitor 변환 검사를 SocketInterop으로 옮겨 공유한다.
- .NET `bindings/dotnet/src/Zlink/Contracts/Eventing/ZlinkPoll.cs`: one-shot mask 오류 계약 주석을 추가했다. 공개 오류는 `ZlinkConfigException.ErrorCode.InvalidArgument`이며 내부 ConfigResult.InvalidArgument와 대응한다.
- .NET `bindings/dotnet/tests/Zlink.Tests/test_monitor_contract.cs:13`: C++와 같은 inproc/tcp 시나리오에 one-shot mask 검증 및 Clear를 추가했다.
- .NET `bindings/dotnet/samples/SampleCommon/SampleSupport.cs`, `samples/MonitorRecv/Program.cs`, `tests/Zlink.Tests/CoreTestSupport.cs`: monitor sleep 대기를 reusable poller wait와 DONTWAIT 수신으로 바꿨다. 기존 5초 deadline을 유지한다. 샘플의 status fallback은 사용하지 않는다.

검증 및 게이트
- `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=3 bash bindings/cpp/tests/run_tests.sh`: PASS, contract 16/16 + sample-smoke 7/7. 로그: cpp-gate-final.log.
- .NET 실행 전 `export ZLINK_CORE_SOURCE=local; source bindings/tools/local_core_runtime.sh`를 적용한 `bash bindings/dotnet/tests/run_tests.sh`: PASS, tests 200/200(실패 0, skip 0), samples 7/7. 로그: dotnet-gate.log.
- .NET monitor 표적 테스트: 12/12 PASS. 로그: dotnet-monitor.log.
- C++ monitor contract `ctest --test-dir bindings/cpp/build --output-on-failure -R '^test_cpp_contract_monitor$' --repeat until-fail:5`: 5회 모두 PASS; 각 회 inproc/tcp 포함. 로그: cpp-monitor-repeat.log.
- .NET 새 `monitor_poller_mask_and_drain` 테스트 필터: 5회 모두 2/2 PASS(inproc/tcp). 로그: dotnet-monitor-repeat-1.log ~ -5.log.
- `git diff --check`: PASS.
- 최초 C++ 게이트는 새 테스트가 중복 remove에 false를 기대해 실패했다. .NET IPoller는 false를 문서화하지만 C++는 미등록 remove를 예외로 처리한다. 이번 요청에 없는 중복 제거 호출을 새 C++ 테스트에서 제외했고, 요청된 remove 뒤 미전달 assertion은 유지했다. 이후 표적 반복 및 전체 게이트가 통과했다. 최초 .NET 테스트 작성의 PollEvent.Events 오기는 실제 공개 이름 Revents로 수정했다.
- C++는 Core 공유 라이브러리를 IMPORTED target으로 링크했다. .NET은 local_core_runtime.sh가 설정한 로컬 native runtime을 사용했다. core/** 안에서 cmake/build/clean을 실행하지 않았다. 빌드 병렬도는 C++ 3, .NET -m:1이었다.
- detached HEAD 유지. 기존 untracked core/build·core/build-dev symlink 유지. 변경은 bindings/cpp/**·bindings/dotnet/**와 요청된 외부 진행 로그/요약뿐이다. spec 수정 및 commit/push/reset/checkout/stash를 수행하지 않았다.

소유 계층: 바인딩 입력 검증과 공개 등록 표면은 C++/.NET binding이 소유하고 readiness·queue·등록 실행은 Core가 소유한다.
Spec 조항: bindings/doc/spec/README.ko.md:2265–2267 “Poller의 monitor source”; Core core/doc/spec/core/05-polling.ko.md §3 socket monitor source.
교차언어 대조: Java NativePoller.java:435–441의 monitorEvents와 동일하게 mask & ~POLLIN을 검사한다. .NET만 reusable 오버로드를 추가한 이유는 해당 공개 표면이 누락됐기 때문이다. Framework 변경은 없다.
변경 분류: A 계약 적응(명시된 공통 monitor source/mask 계약 반영 및 .NET 공개 오버로드 보완).

Spec gap 여부
- 이 구현을 막는 공통 계약 미정 사항은 없다.
- 언어 spec에 monitor poller 등록/수정/제거의 정확한 시그니처를 명시하는 문서 gap은 있다. 아래는 제안만이며 spec 파일은 변경하지 않았다.
- 별도 기존 불일치: C++ 미등록 remove는 현재 invalid_argument를 던지지만 Core polling §7은 NOT_FOUND를 명시한다. 이번 monitor mask 수정 범위 밖이므로 해당 동작은 변경하지 않았다. 위 중복 remove 실패의 원인과 구분해 후속 검토할 수 있다.

`bindings/doc/spec/dotnet/README.ko.md`에 추가할 문장 제안
KO: `IPoller`는 `void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot)`, `void Modify(ISocketMonitor monitor, PollEventFlags events)`, `bool Remove(ISocketMonitor monitor)`를 제공한다. Monitor mask는 `PollIn` 또는 `None`만 허용하며 다른 bit는 `ZlinkConfigException`의 `ErrorCode.InvalidArgument`로 거절한다. `Wait`는 `PollSourceKind.Socket`과 등록 slot을 반환하고, 호출자는 `monitor.Recv(RecvFlags.DontWait)`가 null을 반환할 때까지 event를 drain한다. 동일한 mask 제한은 `ZlinkPoll.Poll(IReadOnlyList<ISocketMonitor>, IReadOnlyList<PollEventFlags>, Span<PollEventFlags>, int)`에도 적용한다.
EN: `IPoller` provides `void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot)`, `void Modify(ISocketMonitor monitor, PollEventFlags events)`, and `bool Remove(ISocketMonitor monitor)`. Monitor masks accept only `PollIn` or `None`; other bits throw `ZlinkConfigException` with `ErrorCode.InvalidArgument`. `Wait` reports `PollSourceKind.Socket` and the registered slot. The caller drains events with `monitor.Recv(RecvFlags.DontWait)` until it returns null. The same mask restriction applies to `ZlinkPoll.Poll(IReadOnlyList<ISocketMonitor>, IReadOnlyList<PollEventFlags>, Span<PollEventFlags>, int)`.

`bindings/doc/spec/cpp/README.ko.md`에 추가할 문장 제안
KO: `poller_t`는 `void add(socket_monitor_t &monitor_, poll_event_flag_t events_, std::uintptr_t slot_)`, `void modify(socket_monitor_t &monitor_, poll_event_flag_t events_)`, `bool remove(socket_monitor_t &monitor_)`를 제공한다. Monitor mask는 `pollin` 또는 `none`만 허용하며 다른 bit는 `config_error_t(config_result_t::invalid_argument, EINVAL)`로 거절한다. `wait`는 `poll_source_kind_t::socket`과 등록 slot을 반환하고, 호출자는 `monitor.recv(recv_flags_t::dontwait)`가 `nullopt`를 반환할 때까지 event를 drain한다.
EN: `poller_t` provides `void add(socket_monitor_t &monitor_, poll_event_flag_t events_, std::uintptr_t slot_)`, `void modify(socket_monitor_t &monitor_, poll_event_flag_t events_)`, and `bool remove(socket_monitor_t &monitor_)`. Monitor masks accept only `pollin` or `none`; other bits throw `config_error_t(config_result_t::invalid_argument, EINVAL)`. `wait` reports `poll_source_kind_t::socket` and the registered slot. The caller drains events with `monitor.recv(recv_flags_t::dontwait)` until it returns `nullopt`.
