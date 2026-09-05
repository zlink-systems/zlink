# zlink 8개 bindings 교차 parity 조사

## 범위와 판정 기준

- 조사 대상: `bindings/{c,cpp,dotnet,java,node,python,go,rust}`.
- 방법: 소스·spec·package metadata 정적 조사만 수행했다. 빌드와 테스트는 실행하지 않았다.
- C/Go/Rust의 vendored eventing header는 `cmp`로 Core header와 byte-identical임을 확인했다. 이는 빌드/테스트가 아닌 read-only 파일 비교다.
- `있음`: 요청한 의미가 public surface에서 모두 구분되거나 사용할 수 있다.
- `부분`: 내부적으로 가능하거나 타입은 있으나 public usability/분류 중 일부가 빠졌다.
- `없음`: public surface에 해당 경로가 없다.
- Core 기준:
  - monitor identity와 READY edge: `core/doc/spec/core/06-monitoring.ko.md:67-77`, `:79-101`.
  - native monitor ABI: `core/include/zlink/eventing/api.h:14-21`, `:38-53`.
  - wait-token terminal: `core/doc/spec/core/socket/README.ko.md:982-992`.
  - request submit의 `NOT_ADMITTED`, `NOT_CONNECTED`, `BACKPRESSURED`+token→WRITABLE: 같은 파일 `:1018-1055`.
  - typed result/errno 정규화: `core/doc/spec/core/03-errors.ko.md:88-112`, `:334-352`, `:519-531`; `bindings/doc/spec/README.ko.md:4030-4055`, `:4208-4232`.

## 결론

1. Monitor event identity는 8개 binding 모두 공개한다. 현재 native layout도 모두 Core의 800 B / 784 / 792 / 796과 일치한다. 다만 수동 FFI mirror인 .NET·Rust에는 숫자 offset 회귀 검사가 없고, Python 검사는 size만 확인한다.
2. Monitor readiness는 C·C++·.NET에서 명시적으로 사용할 수 있다. Python·Go는 기존 generic/socket API에 monitor가 구조적으로 들어가지만 이름·계약이 이를 말하지 않아 `부분`이다. Java·Node·Rust는 public 등록 경로가 없다.
3. Typed submit/terminal은 C++·.NET·Java·Node·Python·Go가 네 경우를 raw errno 없이 구분한다. C는 wait-token terminal 원인을 `send_terminal_errno`로 구분해야 해 `부분`이다. Rust는 `SubmitResult::NotAdmitted` 타입은 있지만 실제 submit failure를 native result가 아닌 errno로 재분류하며 `EPROTOTYPE`/`ECONNREFUSED`를 매핑하지 않아 `InternalError`로 손실한다. 이는 명시된 계약에 반하는 binding bug다.
4. 패키지 resource를 파일로 추출하는 binding은 Java뿐이다. .NET·Node·Python은 package-local native file을 직접 load하고, Go·Rust는 package-local native file을 link/rpath로 사용한다. C·C++는 Core target/package에 link한다.

## 1. Monitor event identity

Core 공개 struct의 정적 layout은 `event@0(8) + value@8(8) + routing_id@16(256) + local@272(256) + remote@528(256) + connection_id@784(8) + transport_lane@792(4) + flags@796(4) = 800 B`다 (`core/include/zlink/eventing/api.h:38-53`).

| Binding | 판정 및 public identity 근거 | native layout 근거 | spec 조항 |
|---|---|---|---|
| C | **있음** — public C struct에 `uint64_t connection_id`, `uint32_t transport_lane`, `uint32_t flags`: `bindings/c/include/zlink/eventing/api.h:38-53` | C ABI 자체가 동일 field sequence이므로 **800 / 784 / 792 / 796** | Core 의미 조항은 있음. C spec은 `connection_id` 용도만 명시: `bindings/doc/spec/c/README.ko.md:434-439`; lane/READY flag와 숫자 layout은 없음(부분). |
| C++ | **있음** — `monitor_event_t`가 세 필드를 노출: `bindings/cpp/include/zlink/Contracts/Eventing/events.hpp:49-100`; 변환: `bindings/cpp/src/Runtime/Eventing/monitor.cpp:20-34` | native `zlink_monitor_event_t`를 Core header에서 직접 사용하므로 **800 / 784 / 792 / 796** | C++ spec은 `connection_id`만 명시: `bindings/doc/spec/cpp/README.ko.md:775-780`; lane/READY flag와 숫자 layout 없음(부분). |
| .NET | **있음** — `MonitorEvent.ConnectionId/TransportLane/Flags`: `bindings/dotnet/src/Zlink/Contracts/Eventing/Monitor.cs:35-53`; 변환: `bindings/dotnet/src/Zlink/Runtime/Eventing/MonitorConverters.cs:10-28` | `[StructLayout(Sequential)]` mirror와 256 B routing id: `bindings/dotnet/src/Zlink/Runtime/Native/NativeTypes.cs:17-22`, `:46-57`. 정적 계산은 **800 / 784 / 792 / 796**; 숫자 assertion은 없음. | .NET spec은 `connection_id`만 명시: `bindings/doc/spec/dotnet/README.ko.md:802-805`; lane/READY flag와 숫자 layout 없음(부분). |
| Java | **있음** — native 값 세 개를 public event로 materialize: `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/Native.java:1181-1212` | layout: `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/NativeLayouts.java:249-273`; **800 / 784 / 792 / 796** 숫자 회귀 검사: `bindings/java/src/test/java/systems/zlink/runtime/nativeapi/NativeLayoutsTest.java:7-14` | Java spec은 `connectionId`만 명시: `bindings/doc/spec/java/README.ko.md:1088-1092`; lane/READY flag와 숫자 layout 없음(부분). |
| Node | **있음** — `MonitorEvent.connectionId/transportLane/flags`: `bindings/node/src/zlink/contracts/eventing/monitor.ts:35-59`, `:128-154`; materialize: `bindings/node/src/zlink/runtime/eventing/monitor_event_state.ts:10-32` | N-API가 `zlink.h`를 include하고 (`bindings/node/native/src/addon_common_api.h:5-12`) Core C struct를 직접 수신·project: `bindings/node/native/src/addon_core.cc:827-860`, `:3235-3268`; 따라서 **800 / 784 / 792 / 796**. 별도 mirror가 없음. | Node spec은 `connectionId`만 명시: `bindings/doc/spec/node/README.ko.md:869-873`; lane/READY flag와 숫자 layout 없음(부분). |
| Python | **있음** — public event class의 세 field: `bindings/python/src/zlink/contracts/eventing/monitor.py:108-141`; recv 변환: `bindings/python/src/zlink/_runtime/eventing/monitor.py:109-116` | ctypes mirror: `bindings/python/src/zlink/_native/ffi.py:18-20`, `:45-55`; size/alignment **800/8** 검사: `bindings/python/tests/test_native_contract.py:23-32`. field sequence의 정적 offset은 **784 / 792 / 796**이나 offset assertion은 없음. | Python spec은 `connection_id`만 명시: `bindings/doc/spec/python/README.ko.md:284-288`; lane/READY flag와 숫자 layout 없음(부분). |
| Go | **있음** — public alias가 세 field를 노출하고 native 변환이 채움: `bindings/go/internal/native/monitor.go:101-142`, `:387-397`; `bindings/go/contracts/eventing.go:7-25` | cgo가 C header의 `C.zlink_socket_monitor_event_t`를 직접 사용: `bindings/go/internal/native/monitor.go:335-350`; header `bindings/go/include/zlink/eventing/api.h:38-53`. 따라서 **800 / 784 / 792 / 796**. | Go spec의 monitor 절 `bindings/doc/spec/go/README.ko.md:177-200`에는 identity 세 필드와 숫자 layout이 없음. |
| Rust | **있음** — public `MonitorEvent` 세 field: `bindings/rust/src/contracts/eventing/monitor.rs:95-114`; 변환: `bindings/rust/src/runtime/eventing/monitor.rs:19-30` | 수동 `#[repr(C)]` mirror: `bindings/rust/src/runtime/native/ffi.rs:429-442`. 정적 계산은 **800 / 784 / 792 / 796**; 숫자 assertion은 없음. | Rust spec은 `connection_id`만 명시: `bindings/doc/spec/rust/README.ko.md:723-728`; lane/READY flag와 숫자 layout 없음(부분). |

공통 binding spec의 `MonitorEvent` 표는 `bindings/doc/spec/README.ko.md:2248-2259`에서 `value`를 `uint32`로 잘못 적고 `connection_id`, `transport_lane`, `flags`를 모두 누락한다. 구현은 맞지만 공통 spec이 Core보다 뒤처진 상태다.

## 2. Poller monitor 등록

Core socket spec은 monitor handle이 `POLLIN` readiness를 내며 `zlink_socket_monitor_recv()`로 drain한다고 명시한다 (`core/doc/spec/core/socket/README.ko.md:62-71`). 그러나 polling 소유 spec은 source를 raw socket/FD/timer 세 종류로만 한정한다 (`core/doc/spec/core/05-polling.ko.md:18-23`, `:43-52`). 따라서 binding별 signature를 강제할 normative 조항은 현재 없다.

| Binding | 판정 및 public 경로 | monitor drain 방식 | binding spec |
|---|---|---|---|
| C | **있음** — generic `void *socket_` 등록/수정/제거: `bindings/c/include/zlink/eventing/api.h:239-246`; 실제 monitor 등록 예: `bindings/c/samples/sample_common.h:127-164`, `:168-177` | poller `POLLIN` 후 `zlink_socket_monitor_recv(..., DONTWAIT)`로 drain: 같은 sample `:147-155` | C spec은 monitor pull과 일반 poller를 설명하지만 monitor registration signature는 없음: `bindings/doc/spec/c/README.ko.md:302-303`, `:434-439`. |
| C++ | **있음** — `add/modify/remove(socket_monitor_t&)`: `bindings/cpp/include/zlink/Contracts/Eventing/poller.hpp:36-49`; 구현 `bindings/cpp/src/Runtime/Eventing/poller.cpp:637-674` | ready event 뒤 `socket_monitor_t::recv` | C++ spec은 poller/monitor를 열거하고 runtime drain을 설명하지만 이 overload signature는 없음: `bindings/doc/spec/cpp/README.ko.md:571-571`, `:661-668`. |
| .NET | **있음** — one-shot `ZlinkPoll.Poll(IReadOnlyList<ISocketMonitor>, ...)`: `bindings/dotnet/src/Zlink/Contracts/Eventing/ZlinkPoll.cs:35-57`; native poll 변환: `bindings/dotnet/src/Zlink/Runtime/Eventing/ZlinkPoll.Core.cs:119-160` | ready monitor에 `Receive`/`TryReceive` 호출. one-shot이므로 별도 remove 없음. | public facade 존재만 기술하며 monitor overload signature는 없음: `bindings/doc/spec/dotnet/README.ko.md:425-430`. |
| Java | **없음** — `Poller` overload는 `Socket`, fd, timer뿐: `bindings/java/src/main/java/systems/zlink/contracts/eventing/Poller.java:8-34` | sample은 `recv(DONT_WAIT)` + `status()` + sleep loop 또는 blocking `recv()`: `bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/SampleSupport.java:100-143` | monitor poller signature 없음. Java spec은 socket poller wait 경계만 설명: `bindings/doc/spec/java/README.ko.md:175-184`. |
| Node | **없음** — public `Poller`는 `BaseSocket`/timer/fd만 수용: `bindings/node/src/zlink/contracts/eventing/poller.ts:39-58`; runtime `BasePollable = BaseSocket`: `bindings/node/src/zlink/runtime/eventing/poller.ts:22-25` | monitor를 우회 전달해도 `completionOwnerOf`가 owner 부재로 throw: `bindings/node/src/zlink/runtime/eventing/poller.ts:194-200`, `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:711-714`. sample은 DONTWAIT+timer loop: `bindings/node/samples/sample_support.ts:31-43`. | monitor registration signature 없음; eventing 역할만 열거: `bindings/doc/spec/node/README.ko.md:285-285`, `:335-355`. |
| Python | **부분** — Protocol은 `add_socket/modify_socket/remove_socket`로만 선언: `bindings/python/src/zlink/contracts/eventing/poller.py:65-118`; runtime은 임의 객체의 `_handle`을 써 monitor도 동작 가능한 구조: `bindings/python/src/zlink/_runtime/eventing/poller.py:80-95`, `:109-139` | 숨은 `add_socket(monitor, POLLIN, slot)` 경로 또는 sample의 `status()`+sleep 뒤 `recv()`: `bindings/python/samples/sample_support.py:24-30` | monitor poller signature/허용 타입 없음: `bindings/doc/spec/python/README.ko.md:39-54`, `:172-179`. |
| Go | **부분** — `AddSocket/ModifySocket/RemoveSocket`가 exported `SocketTarget`을 받고: `bindings/go/internal/native/utility.go:24-26`, `bindings/go/internal/native/poller_timer.go:215-311`; `*SocketMonitor`가 `raw()`를 구현해 실제로 수용: `bindings/go/internal/native/monitor.go:320-324` | `AddSocket(monitor, PollIn, slot)` 뒤 `Recv(DONTWAIT)` 가능하나 이름/문서에서 발견하기 어렵다. | spec은 “socket, fd, timer”만 명시: `bindings/doc/spec/go/README.ko.md:198-200`; monitor 허용 signature 없음. |
| Rust | **없음** — sealed `Pollable`은 built-in socket source 계약이고 `Poller`는 `add/modify/remove_socket`만 제공: `bindings/rust/src/contracts/eventing/poller.rs:25-34`, `:95-177`; `SocketMonitor`의 `Pollable` 구현 없음 | sample은 blocking `monitor.recv()` loop: `bindings/rust/samples/sample_support.rs:43-63` | monitor registration signature 없음; eventing 역할만 열거: `bindings/doc/spec/rust/README.ko.md:492-492`. |

## 3. Typed submit/error result

요청한 네 상태의 canonical typed 이름은 `BACKPRESSURED`, `NOT_CONNECTED`, `NOT_ADMITTED`, wait-token terminal의 `NOT_FOUND`/`TERMINATED`다. `BACKPRESSURED`는 nonzero wait token을 만들고 WRITABLE에서 재제출하며, terminal raw errno는 high-level binding에서 `NOT_FOUND` 또는 `TERMINATED`로 정규화해야 한다.

| Binding | 판정 | 네 경우의 public typed 이름과 근거 | spec |
|---|---|---|---|
| C | **부분** | immediate submit은 `ZLINK_SUBMIT_BACKPRESSURED`, `ZLINK_SUBMIT_NOT_CONNECTED`, `ZLINK_SUBMIT_NOT_ADMITTED`, `ZLINK_SUBMIT_NOT_FOUND`, `ZLINK_SUBMIT_TERMINATED`: `bindings/c/include/zlink_errno.h:100-124`. 그러나 wait token은 `ZLINK_SEND_TERMINAL` 뒤 `send_terminal_errno`의 `ENOENT` 대 lifecycle errno를 caller가 읽어야 함: `bindings/c/include/zlink/socket/api.h:48-61`. | typed submit enum은 공통/Core spec에 있음. C completion ABI 자체가 raw terminal errno를 요구하므로 “raw errno 없이”는 계약 미정. |
| C++ | **있음** | `submit_error_t`: `bindings/cpp/include/zlink/Contracts/Errors/errors.hpp:68-86`; `submit_result_t::{backpressured,not_connected,not_admitted,not_found,terminated}`: `bindings/cpp/include/zlink/Contracts/Sockets/results.hpp:73-88`; terminal `ENOENT→not_found`, `ETERM/ESHUTDOWN→terminated`: `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:51-58`, `:343-356`. | 공통 spec의 C++ `submit_error_t` 계약 있음: `bindings/doc/spec/README.ko.md:4282-4295`. |
| .NET | **있음** | `ZlinkSubmitException` + `SubmitResult.{Backpressured,NotConnected,NotAdmitted,NotFound,Terminated}`: `bindings/dotnet/src/Zlink/Contracts/Errors/SubmitResult.cs:8-53`, `bindings/dotnet/src/Zlink/Contracts/Errors/TypedExceptions.cs:8-100`; terminal errno 정규화: `bindings/dotnet/src/Zlink/Runtime/Errors/ZlinkException.Native.cs:217-235`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:936-945`, `:1240-1250`. | 공통 spec에 .NET exception+Code 계약 있음: `bindings/doc/spec/README.ko.md:4287-4295`. |
| Java | **있음** | `ZlinkSubmitException` + `SubmitResult.{BACKPRESSURED,NOT_CONNECTED,NOT_ADMITTED,NOT_FOUND,TERMINATED}`: `bindings/java/src/main/java/systems/zlink/contracts/sockets/SocketEnums/SubmitResult.java:7-25`, `bindings/java/src/main/java/systems/zlink/contracts/errors/Errors/ZlinkSubmitException.java:7-19`; 전 상태 매핑: `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/NativeSubmitErrors.java:14-57`; terminal 처리: `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1092-1105`, `:1420-1437`. | 공통 spec에 Java exception+code 계약 있음: `bindings/doc/spec/README.ko.md:4287-4295`. |
| Node | **있음** | `SubmitError` + `SubmitResult.{Backpressured,NotConnected,NotAdmitted,NotFound,Terminated}`: `bindings/node/src/zlink/contracts/errors/results.ts:3-20`, `bindings/node/src/zlink/contracts/errors/errors.ts:24-58`; submit/wait 처리: `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:233-260`, `:525-559`; errno 정규화: `bindings/node/src/zlink/runtime/errors/error_mapping.ts:36-59`. | 공통 spec에 Node `SubmitError.code` 계약 있음: `bindings/doc/spec/README.ko.md:4287-4295`. |
| Python | **있음** | `SubmitError` + `SubmitResult.{BACKPRESSURED,NOT_CONNECTED,NOT_ADMITTED,NOT_FOUND,TERMINATED}`: `bindings/python/src/zlink/contracts/sockets/codes.py:35-50`, `bindings/python/src/zlink/contracts/errors/errors.py:28-62`; terminal 정규화/capture: `bindings/python/src/zlink/_runtime/messaging/routed_async.py:818-873`. | 공통 spec에 Python `SubmitError.code` 계약 있음: `bindings/doc/spec/README.ko.md:4287-4295`. |
| Go | **있음** | public alias `*SubmitError` + `Submit{Backpressured,NotConnected,NotAdmitted,NotFound,Terminated}`: `bindings/go/contracts/errors.go:7-59`; 구현 값/Code: `bindings/go/internal/native/result_codes.go:30-47`, `bindings/go/internal/native/error.go:16-35`, `:69-87`; terminal 정규화: `bindings/go/internal/native/completion_owner.go:724-750`, `:917-936`. | 공통 spec에 Go `*SubmitError.Code()` 계약 있음: `bindings/doc/spec/README.ko.md:4287-4295`. |
| Rust | **부분 / bug** | `SubmitError`와 `SubmitResult::NotAdmitted` 타입 자체는 존재: `bindings/rust/src/contracts/errors/results.rs:1-34`, `bindings/rust/src/contracts/errors/errors.rs:8-34`, `:58-69`. 하지만 submit rc를 버리고 `last_errno()`로 재분류: `bindings/rust/src/runtime/errors/native_errors.rs:123-131`; mapper는 `EPROTOTYPE`/`ECONNREFUSED`를 다루지 않음: `:11-27`; async send와 request의 no-token failure가 이를 사용: `bindings/rust/src/runtime/messaging/operations/send_ops.rs:397-408`, `bindings/rust/src/runtime/messaging/operations/routed_async.rs:294-320`. 따라서 `NOT_ADMITTED`가 `InternalError`로 손실된다. `BACKPRESSURED`+token은 두 파일 `:397-399`, `:298-303`, terminal `ENOENT`/close는 `bindings/rust/src/internal/completion_owner.rs:721-733`에서 typed 처리된다. | 공통 spec은 모든 enum 값의 무누락 매핑을 강제: `bindings/doc/spec/README.ko.md:4030-4050`, `:4210-4228`. 명백한 구현 결함. |

추가로 Rust의 errno mapper는 `ENOBUFS`를 `OutOfMemory`로 분류한다 (`bindings/rust/src/runtime/errors/native_errors.rs:25`). Core errors spec은 backpressure의 대표 errno에 `ENOBUFS`도 포함한다 (`core/doc/spec/core/03-errors.ko.md:334-352`). native typed rc를 그대로 사용하도록 고치는 편이 개별 errno 표를 계속 복제하는 것보다 계약에 맞다.

## 4. 네이티브 라이브러리 로딩

| Binding | 방식과 근거 | 추출/누수·캐시 판정 | spec |
|---|---|---|---|
| C | Core build target/imported library에 직접 link: `bindings/c/CMakeLists.txt:67-116`, `:125-131` | 추출 없음; binding package 자체의 native cache 없음 | C spec에 loader/cache 정책 없음. |
| C++ | Core package/build runtime을 resolve하여 link: `bindings/cpp/CMakeLists.txt:126-145`, `:184-198`, `:294-296` | 추출 없음; binding cache 없음 | C++ spec에 loader/cache 정책 없음. |
| .NET | NuGet `runtimes/<rid>/native`에 bundle: `bindings/dotnet/src/Zlink/Zlink.csproj:15-29`; candidate를 `NativeLibrary.Load(path)`로 직접 load: `bindings/dotnet/src/Zlink/Runtime/Native/NativeLibraryLoader.cs:36-49`, `:87-100`, `:154-175` | 추출·temp·cache 없음 | package layout 조항 있음: `bindings/doc/spec/dotnet/README.ko.md:78-82`; cache 정책 불필요. |
| Java | resource를 파일로 추출 후 `System.load`: `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/LibraryLoader.java:65-78` | **유일한 추출형.** resource SHA-256 디렉터리 cache: `:99-124`, 기본 `~/.cache/zlink/native`: `:203-208`; commit `37af8073a7`. 영속 cache라 JVM별 temp 복사는 제거됐지만 eviction이 없어 content version 수만큼 누적된다. cache 실패 시 `zlink-native-*` temp+`deleteOnExit`: `:109-117`, `:158-172`; crash/SIGKILL이면 남을 수 있다. 기존 cache hit는 hash 재검증 없이 size만 검사: `:210-211`. | Java spec은 native artifact lookup 소유 위치만 명시: `bindings/doc/spec/java/README.ko.md:152-166`; 추출 위치·cache·eviction/fallback 계약 없음. |
| Node | npm `prebuilds/**/*` bundle: `bindings/node/package.json:26-36`; package-local `.node`를 `require`: `bindings/node/src/zlink/runtime/native/native_load_paths.ts:22-34`, `bindings/node/src/zlink/runtime/native/native.ts:13-31` | 추출/cache 없음. Linux soname이 없으면 package prebuild dir 안에 symlink를 만들 수 있음: `bindings/node/src/zlink/runtime/native/native_load_paths.ts:68-95`; temp 누수는 아니나 read-only 설치에서 민감한 경로다. | prebuild 위치는 spec에 있음: `bindings/doc/spec/node/README.ko.md:88-91`; symlink/write 정책은 없음. |
| Python | wheel에 `zlink/native/<platform>` bundle: `bindings/python/setup.py:76-84`, `:147-167`; package 경로를 `ctypes.CDLL`로 직접 load: `bindings/python/src/zlink/_native/_native_loader.py:58-100` | 추출·temp·cache 없음 | native target/ownership은 있음: `bindings/doc/spec/python/README.ko.md:17-17`, `:99-103`; direct-load 정책은 없음. |
| Go | module `native/<platform>`를 cgo `-L`/rpath로 직접 link: `bindings/go/internal/native/ffi.go:5-11` | 추출·temp·cache 없음 | module-local runtime 계약 있음: `bindings/doc/spec/go/README.ko.md:249-262`. |
| Rust | crate가 `native/**`를 포함: `bindings/rust/Cargo.toml:9-16`; `build.rs`가 platform dir를 골라 dylib link/rpath 설정: `bindings/rust/build.rs:35-50`, `:100-105` | 추출·temp·cache 없음 | artifact 위치는 있음: `bindings/doc/spec/rust/README.ko.md:73-74`, `:192-192`; runtime cache 정책 불필요. |

## Binding별 gap과 예상 변경 범위

### C

- 구현 identity/poller gap 없음.
- high-level과 동일하게 wait-token terminal을 raw errno 없이 구분하려면 C completion ABI에 typed terminal reason/result를 추가해야 한다. 예상 범위는 `core/include/zlink/socket/api.h`, Core socket/errors spec, `bindings/c/include/zlink/socket/api.h`, C spec과 completion contract tests다. 이는 ABI/계약 결정 전에는 binding bug로 볼 수 없다.
- monitor identity/lane/flags 및 monitor poller 허용을 C spec에 명시해야 한다.

### C++

- 네 표면의 구현 gap은 확인되지 않았다.
- `socket_monitor_t` poller overload와 monitor identity 세 필드를 C++ spec에 명시해야 한다.
- C header를 직접 사용하므로 별도 layout mirror bug 가능성은 낮다.

### .NET

- 네 표면의 기능 gap은 확인되지 않았다. monitor readiness는 reusable `IPoller`가 아니라 `ZlinkPoll.Poll` one-shot surface다.
- 수동 `StructLayout` drift 방지를 위해 예상 범위 `bindings/dotnet/tests/Zlink.Tests/`에 `Unsafe.SizeOf<ZlinkMonitorEvent>() == 800`과 `Marshal.OffsetOf` 784/792/796 회귀 검사를 추가하는 것이 필요하다.
- monitor Poll overload와 identity/flags를 .NET spec에 명시해야 한다.

### Java

- monitor를 public Poller source로 add/modify/remove할 수 없다. 예상 범위: `bindings/java/src/main/java/systems/zlink/contracts/eventing/Poller.java`, `bindings/java/src/main/java/systems/zlink/runtime/eventing/NativePoller.java`, monitor handle adapter, contract tests와 samples.
- native cache는 기능상 parity gap이 아니라 Java packaging 제약에 따른 구조 차이다. 다만 eviction/cleanup, temp fallback leak 허용, cache hit의 content 재검증 여부는 spec 결정이 필요하다. 예상 범위: `LibraryLoader.java`, `LibraryLoaderTest.java`, Java spec.

### Node

- monitor를 Poller source type에 포함해야 한다. 예상 범위: `bindings/node/src/zlink/contracts/eventing/poller.ts`, `bindings/node/src/zlink/runtime/eventing/poller.ts`, monitor native handle/registration bookkeeping, tests와 sample helper. `POLLCOMPLETION` owner 로직은 monitor `POLLIN`에는 적용하지 않아야 한다.
- package dir symlink 생성이 필요한지, read-only install에서도 loader가 성립해야 하는지 loader 계약 결정이 필요하다.

### Python

- 현재 `add_socket(monitor, ...)`가 구조적으로 동작한다. public typing/문서에 monitor를 허용하거나 `add_monitor/modify_monitor/remove_monitor`를 추가해 발견 가능한 계약으로 만들어야 한다. 예상 범위: `bindings/python/src/zlink/contracts/eventing/poller.py`, `_runtime/eventing/poller.py`, contract tests, Python spec/sample helper.
- ctypes test에 `connection_id/transport_lane/flags` offset 784/792/796 assertions를 추가해야 한다.

### Go

- `SocketTarget` 덕분에 monitor가 동작하지만 `AddSocket` 이름과 spec으로는 발견하기 어렵다. `AddMonitor/ModifyMonitor/RemoveMonitor` facade 또는 명시적 문서 계약 중 하나가 필요하다. 예상 범위: `bindings/go/internal/native/poller_timer.go`, public alias/export layer, tests, Go spec.
- cgo가 C header를 직접 사용하므로 별도 manual layout 수정은 필요 없다.

### Rust

- **binding bug:** async/sync submit에서 native typed rc를 보존하고 `NOT_ADMITTED`를 그대로 `SubmitResult::NotAdmitted`로 내야 한다. 예상 범위: `bindings/rust/src/runtime/errors/native_errors.rs`, `runtime/messaging/operations/send_ops.rs`, `runtime/messaging/operations/routed_async.rs` 및 같은 `check_submit_rc`/`submit_error_from_errno` 호출 경로, submit/request tests.
- monitor를 Poller에 넣을 public path가 없다. 예상 범위: `bindings/rust/src/contracts/eventing/poller.rs`, `runtime/eventing/poller.rs`, `Pollable` sealing/handle 변환, tests와 sample helper.
- 수동 `repr(C)` drift 방지를 위해 800/784/792/796 숫자 layout test가 필요하다.

## 분류

### Spec gap / 사용자 결정 필요

1. **공통 MonitorEvent 표가 stale** — `bindings/doc/spec/README.ko.md:2248-2259`가 Core의 `connection_id`, `transport_lane`, `flags`를 누락하고 `value` 폭도 잘못 적는다. 각 언어 spec도 대부분 `connection_id`만 적는다.
2. **숫자 ABI 계약 부재** — 800 B와 offsets 784/792/796은 public C header로만 정해져 있고 Core/binding `.ko` spec에는 숫자 invariant가 없다. manual FFI mirror의 필수 regression assertion 정책도 없다.
3. **monitor poller 계약 불일치** — Core socket spec은 monitor `POLLIN`을 보장하지만 (`core/doc/spec/core/socket/README.ko.md:62-71`), Core polling spec은 monitor를 source 목록에서 누락한다 (`core/doc/spec/core/05-polling.ko.md:18-23`, `:43-52`). Java/Node/Rust 부재를 bug로 확정하기 전에 공통 signature와 one-shot/reusable 양쪽 기대를 정해야 한다.
4. **C terminal typed reason** — C ABI는 `ZLINK_SEND_TERMINAL + send_terminal_errno`이며, raw errno 없이 `NOT_FOUND`/`TERMINATED`를 구분하는 공통 요구와 맞추려면 ABI 결정이 필요하다.
5. **native loader 정책** — 추출 허용 여부, cache root/eviction/integrity, temp fallback cleanup, read-only package 설치 지원에 대한 공통 또는 언어별 normative 조항이 없다. 특히 Java cache와 Node symlink는 정책 결정 대상이다.

### 근거가 있는데 구현이 빠진 binding bug

1. **Rust `NOT_ADMITTED` 손실** — 공통 spec은 모든 submit enum 값의 1:1 typed 매핑을 강제하고 (`bindings/doc/spec/README.ko.md:4030-4050`, `:4210-4228`), Core는 request-to-DEALER를 `NOT_ADMITTED + EPROTOTYPE`로 정한다 (`core/doc/spec/core/socket/README.ko.md:1018-1022`). Rust는 typed rc를 errno로 다시 분류하고 해당 errno를 매핑하지 않아 `InternalError`가 된다 (`bindings/rust/src/runtime/errors/native_errors.rs:11-27`, `:123-131`). 분류: **B 기존 결함**.

Monitor-poller의 Java/Node/Rust 부재와 Python/Go의 불명확한 surface는 현재 normative signature가 없어 “parity gap”이지 확정 binding bug는 아니다. 공통/Core polling spec을 먼저 결정한 뒤 구현 대상으로 승격해야 한다.

## 조사 제약 및 작업 트리

- branch: `main`.
- 파일 수정, 빌드, 테스트 실행 없음.
- 조사 시작 시 source worktree에는 사용자 소유 untracked `doc/plan/c016-worklog/briefs/bindings-parity-survey.prompt`가 있었으며 손대지 않았다.
