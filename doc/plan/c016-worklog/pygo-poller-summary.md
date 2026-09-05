# Python·Go monitor Poller 표면 명시 결과

결과: 완료, EXIT:0. BLOCKERS: 없음. 남은 테스트 실패: 없음.

## 변경

- Python `Poller` Protocol과 runtime에 `add_monitor`, `modify_monitor`, `remove_monitor`를 추가했다. 실제 공개 monitor Protocol 이름인 `MonitorSocket`으로 타입을 명시했다. 공개 공통 `Socket` 타입이 없는 구조이므로 union을 새로 만들지 않고 요청에서 허용한 별칭 방식을 택했다.
- Go에는 `AddMonitor`, `ModifyMonitor`, `RemoveMonitor`를 추가했다. 기존 `SocketTarget`에 대한 문서만 추가하는 대안보다 호출 이름으로 찾기 쉽고, 기존 AddSocket 계열에 위임해 등록 상태나 native 경로를 중복하지 않는다. `SocketTarget` 문서에도 `*SocketMonitor` 수용을 명시했다.
- 기존 Python add/modify/remove_socket, Go Add/Modify/RemoveSocket도 계속 monitor를 수용한다. 두 언어 모두 기존 add/modify 진입점에서 monitor에 POLLIN 이외의 bit가 있으면 typed Config InvalidArgument로 거부한다. 0 mask는 기존처럼 관심 비활성화를 위해 허용한다.
- DEALER→ROUTER 연결 뒤 monitor 등록, slot/source kind/POLLIN, DONTWAIT READY drain, 서버 close 뒤 DISCONNECTED drain, remove 뒤 준비된 monitor 미전달을 inproc/tcp 및 기존 socket API/새 monitor 별칭 모두로 검증했다. POLLOUT, POLLCOMPLETION, POLLIN|POLLOUT의 add/modify 오류도 검증했다.
- Python canonical monitor sample, monitor 대기 공통 helper, examples helper와 Go sample monitor 공통 helper를 Poller 기반으로 변경했다. monitor sleep/status 대기와 Go의 blocking recv 전용 goroutine 대신 readiness 뒤 DONTWAIT 수신을 사용한다.
- Python README와 Go README.godoc에 Poller 절과 사용 문장을 추가했다.

변경 파일:

- `bindings/python/src/zlink/contracts/eventing/poller.py`
- `bindings/python/src/zlink/_runtime/eventing/poller.py`
- `bindings/python/tests/test_monitor_poller.py` (신규)
- `bindings/python/samples/monitor_recv_sample.py`
- `bindings/python/samples/sample_support.py`
- `bindings/python/examples/sample_common.py`
- `bindings/python/README.md`
- `bindings/go/internal/native/poller_timer.go`
- `bindings/go/internal/native/utility.go`
- `bindings/go/monitor_poller_test.go` (신규)
- `bindings/go/samples/internal/samplecommon/samplecommon.go`
- `bindings/go/README.godoc.md`

## 소유권과 계약 근거

소유 계층: binding은 공개 이름·typing·입력 마스크 검증을 소유하고, Core는 monitor 등록·readiness·drain 동작을 소유한다. 새 별칭은 기존 socket 등록 경로에 위임한다.
Spec 조항: `bindings/doc/spec/README.ko.md:2267` 및 `core/doc/spec/core/05-polling.ko.md` §3 socket monitor 행.
교차언어 대조: C++ `bindings/cpp/src/Runtime/Eventing/poller.cpp:637-674`의 socket_monitor_t add/modify/remove도 native socket 등록 경로를 사용한다. Python·Go를 같은 방식으로 맞췄다. Python drain 종료는 None, Go는 `*RecvError.Result == RecvNoData`라는 기존 반환 계약 차이가 있다.
변경 분류: A — 명시된 공통 binding 계약과 요청에 대한 표면·입력 검증 적응. Framework runtime 변경 없음.

참고: 현재 Core 0.17.0은 monitor POLLOUT 등록 자체를 성공시킨다(최초 신규 public API 테스트에서 확인). 요청한 typed InvalidArgument 보장은 binding 입력 검증으로 제공한다. Core 코드나 readiness 처리는 변경하지 않았다.

## 테스트·게이트

모든 최종 검증 PASS:

- Python in-place extension: 기존 venv `/home/hep7hep7/project/zlink-work/c016/python-venv` 사용, local_core_runtime 환경에서 `python setup.py build_ext --inplace` 성공. 최초에는 local package runtime payload가 없어 실패했으나 `bindings/python/src/zlink/native/linux-x86_64/`에 메인 Core를 가리키는 ignored symlink를 준비한 뒤 성공했다. Core build 작업 없음.
- Python 공식: `PYTHON_EXECUTABLE=/home/hep7hep7/project/zlink-work/c016/python-venv/bin/python bindings/python/tests/run_tests.sh` → 190 passed, 4 subtests passed; samples 7/7.
- Python 신규 반복: local Core 환경과 `PYTHONPATH=bindings/python/src`에서 `python -m pytest bindings/python/tests/test_monitor_poller.py -q`를 5회 → 매회 10 passed.
- Go 공식: `CGO_LDFLAGS=-L<작업트리>/core/build/lib LD_LIBRARY_PATH=<작업트리>/core/build/lib bindings/go/tests/run_tests.sh` → `go test ./...`, `go vet ./...`, raw contract/hot-path guards 모두 PASS; samples 7/7.
- Go 신규 반복: 같은 환경에서 `cd bindings/go; go test . -run TestMonitorPoller -count=5` → PASS (각 회 inproc/tcp × 기존/별칭 및 오류 검증).
- Python examples `sample_common.wait_connected`를 실제 TCP PAIR monitor 두 개로 별도 스모크 → PASS.
- `git diff --check` → PASS.
- 실제 Python native loader 경로 `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`, `zlink.version() == (0, 17, 0)` 확인. Go 링크·로드 경로도 같은 Core build/lib를 환경으로 지정했다.

로그:

- `/home/hep7hep7/project/zlink-work/c016/pygo-python-build.log`
- `/home/hep7hep7/project/zlink-work/c016/pygo-python-gate.log`
- `/home/hep7hep7/project/zlink-work/c016/pygo-python-repeat.log`
- `/home/hep7hep7/project/zlink-work/c016/pygo-go-gate.log`
- `/home/hep7hep7/project/zlink-work/c016/pygo-go-repeat.log`

## 언어 spec에 추가할 signature 문장(ko/en) — 제안만, spec 파일 수정 없음

대상: `bindings/doc/spec/python/README.ko.md` (영문 대응 문서에도 아래 en 문장 제안)

KO: `Poller.add_monitor(monitor: MonitorSocket, events: PollEventFlag, slot: int) -> None`, `Poller.modify_monitor(monitor: MonitorSocket, events: PollEventFlag) -> None`, `Poller.remove_monitor(monitor: MonitorSocket) -> None`는 socket monitor를 등록·수정·제거하는 공개 별칭이다. 기존 `add_socket/modify_socket/remove_socket`도 monitor를 수용한다. monitor의 유효한 readiness bit는 `POLLIN`뿐이며 다른 bit 요청은 `ConfigError(ConfigResult.INVALID_ARGUMENT)`로 실패한다(0은 관심 비활성화). readiness 뒤 `monitor.recv(flags=RecvFlags.DONT_WAIT)`를 `None`까지 drain하며 monitor를 닫기 전에 등록을 제거한다.

EN: `Poller.add_monitor(monitor: MonitorSocket, events: PollEventFlag, slot: int) -> None`, `Poller.modify_monitor(monitor: MonitorSocket, events: PollEventFlag) -> None`, and `Poller.remove_monitor(monitor: MonitorSocket) -> None` are public aliases for registering, modifying, and removing a socket monitor. The existing `add_socket/modify_socket/remove_socket` methods also accept monitors. `POLLIN` is the only supported monitor readiness bit; requesting other bits raises `ConfigError(ConfigResult.INVALID_ARGUMENT)` (zero disables interest). After readiness, drain `monitor.recv(flags=RecvFlags.DONT_WAIT)` until `None`, and remove the registration before closing the monitor.

대상: `bindings/doc/spec/go/README.ko.md` (영문 대응 문서에도 아래 en 문장 제안)

KO: `(*Poller).AddMonitor(monitor *SocketMonitor, events PollEventFlag, slot uintptr) error`, `(*Poller).ModifyMonitor(monitor *SocketMonitor, events PollEventFlag) error`, `(*Poller).RemoveMonitor(monitor *SocketMonitor) error`는 기존 `AddSocket/ModifySocket/RemoveSocket`에 위임하는 공개 별칭이며 `*SocketMonitor`는 기존 `SocketTarget`도 만족한다. monitor의 유효한 readiness bit는 `PollIn`뿐이며 다른 bit 요청은 `*ConfigError{Result: ConfigInvalidArgument}`로 실패한다(0은 관심 비활성화). readiness 뒤 `monitor.Recv(RecvFlagsDontWait)`를 `*RecvError.Result == RecvNoData`까지 drain하며 monitor를 닫기 전에 등록을 제거한다.

EN: `(*Poller).AddMonitor(monitor *SocketMonitor, events PollEventFlag, slot uintptr) error`, `(*Poller).ModifyMonitor(monitor *SocketMonitor, events PollEventFlag) error`, and `(*Poller).RemoveMonitor(monitor *SocketMonitor) error` are public aliases delegating to `AddSocket/ModifySocket/RemoveSocket`; `*SocketMonitor` also satisfies the existing `SocketTarget`. `PollIn` is the only supported monitor readiness bit; requesting other bits returns `*ConfigError{Result: ConfigInvalidArgument}` (zero disables interest). After readiness, drain `monitor.Recv(RecvFlagsDontWait)` until `*RecvError.Result == RecvNoData`, and remove the registration before closing the monitor.

## BLOCKERS

없음. 금지된 spec 경로와 Core는 수정하지 않았고 commit/push/reset/checkout/stash를 실행하지 않았다. 초기부터 있던 untracked `core/build`, `core/build-dev` symlink는 유지했다. 작업 branch는 요청대로 detached 상태로 유지했다.
