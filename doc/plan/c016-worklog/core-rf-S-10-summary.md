# S-10 요약 — 핫 경로의 동적 TLS(`__tls_get_addr`) 제거

## 결과 (수치)

| 항목 | before (S-A 실측) | after (S-10) |
|---|---|---|
| `__tls_get_addr` 호출/메시지 (callgrind 축소셀 1024 B) | **31.9** (1,839,732회 / 57,672 msg) | **0.0001** (8회 / 75,664 msg) |
| 그 8회의 출처 | — | 전부 libstdc++의 `__cxa_get_globals`(예외 처리 초기화). libzlink 코드 경로 **0회** |
| `__tls_get_addr` 자체비용 | 383 Ir/msg (3.43 %) | 0 |
| 총 Ir/메시지 (축소셀, 서버 기동분 3.45 M 차감) | 11,096 | **10,253** (**−843 Ir/msg, −7.6 %**) |
| libzlink.so PT_TLS memsz | 0x128 (296 B) | 0x138 (312 B) |
| GD 재배치(`DTPMOD`/`DTPOFF`) | 15 | **0** |
| IE 재배치(`TPOFF`) | 0 | **15** |
| `__tls_get_addr` 심볼 참조 | 있음 | **없음**(`readelf -sW` 0건) |

자체비용 383 Ir/msg만 사라진 게 아니라 호출측의 인자 셋업·PLT 왕복까지 함께 없어져 843 Ir/msg가 줄었다.

## 변경 파일

- `core/CMakeLists.txt` (+18행, `if (NOT MSVC)` 블록 안). 유일한 변경.

새 CMake 옵션·플래그·환경변수·소스 규칙을 **추가하지 않았다**. 컴파일러가 플래그를 받으면 무조건 적용한다(기존 `-Wno-tautological-constant-compare` 처리와 동일한 형태).

## TLS 사용처 전수 목록 (7곳) — 메시지당 호출 분포

callgrind 호출 그래프에서 `__tls_get_addr` PLT 스텁의 호출자를 역추적해 얻었다(57,672 msg 기준).

| file:line | 변수 | 호출/msg |
|---|---|---|
| `core/src/runtime/core/recv_tls_view.hpp:36` | `recv_tls_view::storage()`의 `tls` (+ 가드 변수) | `storage()` 9.61, `reset()` 4.81, `zlink_recv_part` 3.00, `commit()` 3.00 = **20.4** |
| `core/src/runtime/sockets/common/socket_runtime.hpp:839` / `socket_lifecycle_runtime.cpp:54` | `_current_thread_public_api_sync_owner` | `mark_public_api_sync_owned` 2.00, `lock_public_api_sync` 1.21, `unmark` 1.00, `unlock` 0.60 = **4.81** |
| (boost) `asio::detail::keyword_tss_ptr<call_stack<thread_context>>::value_` | asio 스레드 컨텍스트 | `asio_poller_t::loop` 2.15, `tcp_transport_t::async_read_some` 1.00, `reactive_socket_recv_op::do_complete` 1.00 = **4.15** |
| `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:204` | `submit_progress_wait_scope_t::current_owner_socket_tls` | `process_commands` 2.05에 포함 |
| `core/src/runtime/core/msg.cpp:42` | `slice_content_pool` (+ 가드) | 위 경로에 흡수 |
| `core/src/runtime/sockets/common/socket_base_api.cpp:1028` | `tls_completion_drain_owner` | 저빈도 |
| `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:16` | `async_mailbox_dispatch_socket_tls` | `mailbox_t::schedule_if_needed_unlocked` 0.48 |
| (boost beast) `websocket::detail::secure_generate/fast_generate`의 `gen` | 이 시나리오에서 미사용 | 0 |

핵심: **`recv_tls_view` 하나가 전체의 64 %**. 함수 지역 `static thread_local` + 비자명 소멸자라 접근 1회당 **가드 변수 + 본체 = `__tls_get_addr` 2회**가 나가고, `zlink_recv_part` → `commit` → `reset`이 각자 독립적으로 `storage()`를 다시 부른다.

## 설계 비교와 선택 이유

브리프의 세 안을 모두 검토했다.

**(A) 빌드 플래그 `-ftls-model=initial-exec` — 채택.**
- 커버리지 100 %: zlink 자체 TLS 4개 + boost asio의 `keyword_tss_ptr`(4.15회/msg)까지 한 번에 없앤다. (B)·(C)로는 boost 쪽 4.15회를 건드릴 수 없다.
- 추가되는 규칙 0개. 소스 한 줄도 안 바뀌므로 동작 회귀 표면이 없다(주소 계산 모델만 바뀐다).
- 유일한 위험은 **dlopen 시 static TLS 블록 고갈**이다. 실측으로 해소했다(아래).

**(B) 소스 수준 TLS 포인터 1회 캐시 — 기각.**
`recv_tls_view`의 20.4회는 접을 수 있으나(→ 2회 수준), lifecycle 4.81회·boost 4.15회는 남아 31.9 → 약 9회에 그친다. 게다가 "핫 함수는 진입 시 TLS를 지역 변수로 받아 쓴다"는 **새 코딩 규칙**을 도입하고 그 규칙을 지키는지 리뷰로 계속 확인해야 한다(POSDDD: 규칙 수 증가). 이득은 (A)의 30 %인데 규칙과 diff는 (A)보다 훨씬 크다.

**(C) `thread_local` 자체 제거 — 기각(현 job 범위에서).**
`recv_tls_view`는 `zlink_recv_part`가 반환하는 파트 배열의 소유권을 다음 호출까지 유지해야 해서 호출자 스택으로 옮기려면 **공개 C API의 소유권 계약**을 바꿔야 한다(금지 항목). `_current_thread_public_api_sync_owner`·`tls_completion_drain_owner`는 정의상 "이 스레드가 소유자인가"를 묻는 상태라 스레드별이어야만 한다. 즉 제거 가능한 것이 없다.

### (A)의 dlopen 근거 (필수 조사)

- **glibc가 dlopen된 객체에 남겨두는 static TLS surplus 실측** (glibc 2.39, Ubuntu 24.04): `__attribute__((tls_model("initial-exec")))` 블록 크기를 키운 모듈을 dlopen하며 이분 탐색.
  - 순수 C 프로세스: 64·128·256·512·1024·1280·1536·**1664 B OK**, **1792 B 실패**("cannot allocate memory in static TLS block").
  - **CPython(bindings 파이썬 venv, `ctypes.CDLL`)에서도 동일**: 1664 B OK, 1792 B 실패. 파이썬이 이미 올려둔 확장 모듈들이 surplus를 갉아먹지 않았다는 뜻이다.
- **libzlink가 요구하는 양: 312 B** (release, LTO ON: `readelf -lW libzlink.so.0.17.0` → `TLS ... memsz 0x138`). 여유의 **18.8 %**. 나머지 1352 B는 다른 dlopen 모듈 몫으로 남는다.
- **실제 로드 확인**: venv 파이썬에서 `ctypes.CDLL(<worktree>/core/build/lib/libzlink.so.0.17.0)` → 성공, 이어서 `zlink_ctx_new()` → `zlink_ctx_term()`까지 정상(= I/O 스레드가 생성·종료되며 스레드별 static TLS 블록이 실제로 할당된다). node/java/dotnet도 같은 `dlopen` 경로이고 요구량은 동일한 312 B이므로 같은 결론이 적용된다.
- **C 바인딩**: `bindings/c/tests/run_tests.sh` (ZLINK_CORE_SOURCE=local) 6/6 통과.

기록해 둘 조건: 이 여유는 프로세스 전체가 공유한다. 향후 libzlink의 TLS 총량이 수백 바이트대를 크게 넘어서면(예: 스레드별 큰 버퍼를 `thread_local`로 두면) 이 플래그가 dlopen 실패로 이어질 수 있다. 312 B는 그 한계에서 5배 이상 떨어져 있다.

## 실행한 테스트와 남은 실패

| 테스트 | 결과 |
|---|---|
| `ctest --test-dir core/build-dev -R 'recv\|stream\|router\|dealer\|poll'` ×5 (`-j4`) | 3회차에서 `test_close_completion_poller_release` 1회 실패 |
| 위 실패 테스트 단독 재실행 ×3 | 3/3 통과 |
| 같은 패턴 직렬(`-j1`) 1회, 80개 테스트 | 전부 통과 |
| `bindings/c/tests/run_tests.sh` | 6/6 통과 |
| 파이썬 dlopen 경로(venv `ctypes` + ctx 생성/종료) | 통과 |

`test_close_completion_poller_release` 1회 실패는 **다른 job 2개(s2·s4)가 동시에 빌드 중인 상태에서 `-j4`로 돌린 부하 유발 flake**로 판단한다: 단독·직렬 재실행 4회 모두 통과했고, 이번 변경은 소스를 한 줄도 바꾸지 않아(TLS 주소 계산 명령열만 다름) 타이밍 이외의 경로로 이 테스트에 영향을 줄 수 없다.

TSan은 돌리지 않았다 — pipe·engine·mailbox·mutex 소스를 만지지 않았고 변경이 CMake 플래그뿐이라 공통 규칙의 TSan 트리거(해당 소스 수정)에 해당하지 않는다.

## 성능 표 (with_stream, zlink,asio, CCU 1000, runs 1, worktree release lib)

| 셀 | Phase 0 기준 | after run1 | after run2 | asio 기준 | asio run1 | asio run2 |
|---|---|---|---|---|---|---|
| 64 B (kops) | 268.9 | 267.2 | 265.3 | 322.0 | 315.4 | 314.1 |
| 1024 B (kops) | 243.0 | 230.6 | 232.8 | 316.4 | 289.0 | 296.4 |
| 64 KiB (kops) | 30.4 | 29.2 | 30.6 | 39.2 | 38.5 | 38.3 |

측정 시작 load average: run1 `1.11 2.69 5.29`, run2 `1.40 2.46 4.97`. callgrind 셀 `1.35 2.89 5.43`. 모두 `flock .../PERF_LOCK`으로 직렬화했다.

**해석**: 이번 런에서는 **코드가 전혀 바뀌지 않은 asio도 기준값 대비 2~9 % 낮게 나왔다**(1024 B에서 316.4 → 289.0/296.4). 즉 절대값은 Phase 0 측정 당시와 같은 머신 상태가 아니다. 같은 런 안에서만 비교 가능한 zlink/asio 비를 보면 64 B 0.835 → 0.847/0.845, 1024 B 0.768 → 0.798/0.785, 64 KiB 0.776 → 0.758/0.799 로, **회귀는 없고 개선은 단일 런 노이즈에 묻힌다**. 이는 예상과 일치한다: 축소셀에서 명령 수는 확실히 7.6 % 줄었지만(재현 가능한 결정론적 수치), CCU 1000 실규모의 병목은 §4 1·2위인 command 왕복과 syscall이라 명령 수 절감이 처리량으로 바로 환산되지 않는다. 이 job의 성과는 **"메시지당 383 Ir(3.4 %)의 순수 낭비를 결정론적으로 0으로 만들었다"**로 읽어야 하며, 이후 S-1/S-9가 병목을 걷어낸 뒤에 처리량으로 드러난다.

## 재확인한 스펙 절

- `core/doc/spec/core/systems/04-thread-safety.ko.md`, `core/doc/spec/core/systems/10-hot-path.ko.md` 전문 확인. 두 문서 어디에도 TLS 접근 모델·`thread_local`·심볼 재배치에 대한 문장이 없다(둘 다 관측 가능한 동작만 규정한다).
- 이번 변경은 **어떤 `thread_local` 변수의 값·수명·스레드별 격리도 바꾸지 않는다**. 바뀐 것은 같은 변수의 주소를 구하는 명령열뿐이다(`__tls_get_addr` 호출 → 스레드 포인터 + 상수 오프셋). 초기화 시점(가드 변수), 스레드 종료 시 소멸자 호출(`__cxa_thread_atexit`) 모두 그대로다.
- **어느 문장도 다른 동작이 되지 않았다.** completion·READY/DISCONNECTED·POLLIN/POLLOUT level·WRITABLE wake의 순서와 조건은 코드가 바뀌지 않았으므로 그대로다. 공개 헤더·`libzlink.vers`·계약 테스트 기대값은 손대지 않았다.

## 변경 분류

**B (기존 결함)** — 공유 라이브러리 기본 TLS 모델이 general-dynamic이라 핫 경로가 메시지당 31.9회의 불필요한 `__tls_get_addr`을 내던 빌드 구성 결함을, 계약 변경 없이 빌드 플래그 한 줄로 고쳤다.

## 멈춘 지점

- **파이썬 바인딩 전체 테스트(`bindings/python/tests/run_tests.sh`)는 돌리지 못했다.** `bindings/python/build.sh`가 `src/zlink/native/linux-x86_64`에 스테이징된 Core 런타임 페이로드를 요구하는데 worktree에는 없어 `RuntimeError: Core runtime payload is missing`으로 중단된다(이 job의 변경과 무관한 패키징 전제). 대신 브리프가 요구한 **dlopen 경로 자체**를 venv 파이썬의 `ctypes.CDLL` + `zlink_ctx_new/term`으로 직접 확인했고, 여기에 IE-TLS 여유 이분 탐색(1664 B)을 더해 근거로 삼았다. 감독관 게이트에서 파이썬 스모크를 정상 경로로 한 번 돌려주면 좋겠다.
- **커밋하지 않았다.** worktree `~/project/zlink-work/s10` (detached b52c8b1055)에 diff로 남겼다.
