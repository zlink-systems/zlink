# Python single REQREP 비용 진단과 binding 수정 — 2026-09-07

감독자 검토용 작업 기록이다. 대상은 `DEALER_ROUTER_REQREP / tcp / 64 B`,
2-part, active 2초, 미완료 상한 64다. **최종 교차 측정은 17,632.5 → 25,419 ops/s
(+44.2%)이며, binding 단독 효과는 +9.1%다. 10만 ops/s 목표는 미달이다.**
Core·스펙·Go 파일과 commit/push는 건드리지 않았다. 자동 completion 경로의 기존 간헐 실패도
발견했다. 성능 개선과 이 미해결 항목을 함께 검토해야 한다.

## 실행 조건

- `main`에서 시작했다. 기존 Go·Rust·Python perf 수정과 untracked 기록을 보존한다.
- `ZLINK_CORE_SOURCE=release`,
  `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1`.
- `ZLINK_LIBRARY_PATH`와 `LD_LIBRARY_PATH`도 위 prefix의 `lib`로 고정한다.
  Core SHA-256은 `a3e00fd269b2a1c8d66371ac7ae7efd3f6dd25e6ab842484b847352258ea39a2`.
- Python 3.12.3. 기존 D-B126·D-B130 C 확장과 직전 러너 정합 위에서 시작한다.
- 모든 perf 앞에 `bash scripts/perf/wait-for-idle-perf.sh`를 실행한다.
  Go perf가 감지된 계측은 40초 대기한 뒤 실행했다.
- 계측 도구·원본 snapshot·전체 출력은 `/tmp/zlink-python-reqrep/`에 보관한다.
  임시 계측은 최종 runtime에 남기지 않았다. 구간 계측용 C 확장은 별도로 만들고 원래
  최종 확장으로 복구한 뒤 SHA-256 일치를 확인했다.

## 1. 수정 전 측정과 병목

동일 표준 러너 3회: **17,842.5 / 17,130.5 / 18,535.0 ops/s**.
중앙값 17,842.5로 제시된 17,916.5를 재현했다.
원본 출력은 `/tmp/python-reqrep-before.log`다.

### GIL 재획득

`LD_AUDIT`로 `PyEval_RestoreThread` 진입부터 반환까지 monotonic clock으로 측정했다.
Core를 수정하거나 Python의 GIL 설정을 바꾸지 않았다. 계측 실행은 17,928.5 ops/s다.

| 역할 | GIL 재획득 횟수 | 재획득 누적 시간 | 요청당 시간(약 35,900건) | 최장 1회 |
|---|---:|---:|---:|---:|
| replier | 107,737 | 1.408초 | 39.2µs | 3.31ms |
| requester | 170,533 | 0.549초 | 15.3µs | 0.595ms |

이는 두 thread에 걸친 누적 대기이며, 두 값을 합쳐 단일 thread의 CPU 시간으로 해석하면
안 된다. 그러나 replier의 2초 중 1.408초가 GIL 복귀에 쓰였다는 사실은 직접 관측이다.
Core I/O thread 수보다 **Python requester와 replier 사이의 GIL 인계**가 큰 병목이다.
`gil-audit.c`, `gil-audit-before.log`가 재현 자료다.

최종 코드의 같은 계측(`gil-audit-after.log`, 24,956 ops/s)에서는 replier가
103,411회/0.838초, requester가 201,991회/0.597초였다. 약 49,900건 기준 각각
**16.8µs/요청, 12.0µs/요청**이다. replier의 대기는 줄었지만 GIL 인계가 사라진 것은 아니다.

### 요청 수명 구간

`stages.py`는 기존 함수 입출구에서 wall clock과 해당 thread의 CPU clock을 함께 잰다.
계측 비용 때문에 처리량은 14,629.5 ops/s로 낮아진다. 따라서 이 실행은 비용 분해용이고
before/after 처리량 판정에는 쓰지 않는다. 약 29,323건 기준이며 괄호로 표시한 구간은
상위 구간에 포함되므로 중복 합산하지 않는다.

| 구간 | wall / 요청 | thread CPU / 요청 |
|---|---:|---:|
| header stamp | 1.16µs | 0.92µs |
| `start_request` 전체: materialize·entry·admission 등록 | 18.41µs | 13.38µs |
| ↳ `submit_storage`: part별 native 제출 경계 | 10.67µs | 5.61µs |
| public poller `wait` 전체 | 26.82µs | 16.66µs |
| ↳ completion drain | 16.08µs | 11.43µs |
| ↳ reply Message 구성 | 1.32µs | 1.05µs |
| reply 2-part `to_bytes` | 1.19µs | 0.76µs |
| reply part layout 확인 | 0.55µs | 0.35µs |
| reply header decode | 0.72µs | 0.52µs |
| latency 집계 | 0.72µs | 0.53µs |
| reply Message close | 1.26µs | 0.87µs |
| 서버 `router_recv_into` | 18.27µs | 5.52µs |
| 서버 `_reply_payload` | 45.16µs | 13.70µs |

별도 `LD_PRELOAD` 계측에서 C 확장이 직접 호출한 `zlink_request_part`는 68,504회,
누적 94.06ms였다. 2-part 요청당 **2.75µs**다. `zlink_router_recv_part`는 68,507회,
누적 48.33ms로 요청당 1.41µs였다. 이는 native 함수 안의 시간이며 Python↔C 인자 구성과
GIL 복귀는 포함하지 않는다. ctypes가 handle에서 직접 찾은 함수는 이 interpose를
거치지 않으므로 이 계측의 reply/completion 0회는 호출이 없었다는 뜻이 아니다.

이를 보완한 `LD_AUDIT` 계측은 ctypes의 symbol lookup도 가로챈다
(`audit-all.c`, `audit-all-before.log`). 45,472 요청 기준 Core 내부 시간은
request 2-part 합계 1.20µs, reply 2-part 합계 1.21µs, completion recv 0.128µs,
completion close 0.071µs, poller wait 합계 1.69µs/요청이었다. 계측 방식에 따라
GIL 인계 양상이 달라지므로 위 실행의 값을 앞 표에서 직접 빼지는 않는다.
공통으로 관측되는 사실은 native 함수 자체보다 Python 측 경계·GIL 복귀 비용이 크다는 것이다.

payload 구성은 `/tmp`의 계측 전용 C 확장에서 `materialize_payload` 입출구를 직접 잰다.
34,988회/39.449ms, **1.128µs/요청**이었다(`materialize-before.log`). 이는 2-part의
native storage 구성과 입력 복사를 포함한다. 최초 template의 `new_payload`는 active 밖에서
한 번만 실행되고, 매 요청의 header stamp는 앞 표처럼 별도 구간이다.

### Python instruction·함수·C 확장 호출 수

Python 3.12 `sys.monitoring`의 `INSTRUCTION`, `PY_START`, `PY_RESUME`를 C callback으로
센다(`opcount.c`, `opcodes.py`). 최초 Python callback 계측은 timeout을 유발할 정도로
느려 폐기했고, `instructions-native-before.log`와 `instructions-after.log`만 채택했다.
분모는 완료 count가 아니라 제출한 요청 수(14,404 / 18,192)다. 아래 수치는 requester와
replier를 합친 값이며 setup thread는 제외한다. 계측 처리량은 판정용으로 쓰지 않는다.

| 항목 / 요청 | before | after |
|---|---:|---:|
| 실행 Python bytecode, stdlib 포함 | 2,877.0 | 2,244.8 |
| Python 함수 최초 진입, coroutine 재개 제외 | 155.6 | 128.5 |
| cProfile에서 관측한 Python→binding C 확장 호출 | 11.20 | 13.08 |
| 그중 `submit_storage` | 1 | 2 |
| Core request part / reply part | 2 / 2 | 2 / 2 |

C 확장 호출은 줄지 않았다. reply를 C 확장에 연결했고, 서버의 bytes 복사 대신 native
part clone을 사용하기 때문이다. 감소한 것은 Python 코드 실행과 spin, ctypes reply 경계다.
확장 호출 수에는 `zlink._native._zlink_native` 함수·owner 메서드와 `CompletionOwner._drain`을
포함하며, 확장 내부의 C→C 호출은 제외한다. 전체 요청 수명 수치를 D-B130의 one-way
메시지당 Python 함수 9회와 혼동하면 안 된다.

latency 해석도 정정한다. Python 러너는 RTT를 **2로 나눈 값**을 출력한다.
따라서 제시된 17,917 × 1.628ms는 in-flight 29의 근거가 아니고, RTT를 복원하면 약 58이다.
제출 직렬화가 아니라 요청당 처리 비용이 크다는 결론은 동일하다.

## 2. Node와의 구조 대조

`bindings/node/perf/single/perf_socket_reqrep.ts`도 미완료 Promise를 64개까지 채운다.
Node replier는 `worker_threads` worker이며 requester와 별도의 V8 isolate를 사용한다.
Python은 같은 interpreter의 `threading.Thread` 두 개이므로 GIL을 공유한다.
따라서 두 언어를 같은 단일 실행 thread 조건으로 보고 10만 ops/s를 하한으로 삼을 수는 없다.
Node server의 `perf_single_sender_worker.ts:239-267`는 받은 native Message를 reply에 넘긴다.
Node 수치를 이번 작업에서 다시 측정하지는 않았으며, 149,844는 사용자가 제공한 비교값이다.

Python에 추가로 남은 비용은 다음과 같다.

- D-B130의 C 확장 안에서도 completion drain은 ctypes callable을 호출한다.
- 서버 reply는 별도 Python part loop·lambda·ctypes 인자 구성을 거쳤다.
- private asyncio loop에서 request coroutine, shield Future, callback을 실행한다.
- 기존 러너는 매 turn `POLLOUT|POLLCOMPLETION` timeout-0 wait를 반복한다.
- 서버가 받은 part를 bytes로 바꾼 뒤 reply에서 다시 native Message로 구성한다.

single I/O thread는 `PERF_POLICY.md:1550`과 `perf/single/perf_common.py:104` 모두 기본 1이다.
multi의 Python 예외(`PERF_POLICY.md:1050`)는 single의 설정 근거가 아니다.
직전 정합 기록이 Python multi 기본값을 4로 바꿨다는 부분은 현재 예외 조항과 불일치한다.
이번 single 진단에서는 multi 기본값을 변경하지 않는다.
binding reply만 수정한 동일 러너에서 I/O thread 4의 3회 결과는
20,820 / 21,151 / 21,419 ops/s였다. 중앙값 21,151은 I/O thread 1의 당시 중앙값
22,694보다 6.8% 낮다. 따라서 I/O thread 1이 부족해서 생긴 병목이라는 가설은 채택하지 않는다.

## 3. 후보와 판정

| 후보 | 판정·이유 |
|---|---|
| reply를 기존 D-B130 `submit_storage`로 연결 | 채택. part별 Core 호출·GIL 해제·소비 ownership은 유지한다. 별도 제출 loop를 추가하지 않는다. 예비 측정은 +27.2%였지만 최종 binding 묶음 판정은 아래 교차 측정의 +9.1%를 사용한다. |
| RoutingId → ctypes 구조체 → bytes 왕복 제거 | 채택. 기존 `_as_bytes_view`를 사용하고 길이를 한 번 검증한다. 이 변경만의 예비 측정은 24,220 → 25,247 ops/s(+4.2%)이므로 독립적인 5% 이상 효과를 주장하지 않는다. 검증 규칙과 표현 변환이 단순해진다. |
| timeout-0 poll·POLLOUT 반복 제거 | 러너 수정으로 분리. 가이드 §2.3·§4에 어긋난 spin을 없앤다. 자기 private loop의 ready continuation이 없을 때만 POLLCOMPLETION에서 blocking wait한다. 기존 active/drain deadline의 잔여 시간만 사용한다. |
| 받은 part를 bytes로 바꿔 reply하는 경로 제거 | 러너 수정으로 분리. 가이드 §2.4에 따라 받은 part를 public reply에 넘긴다. replier는 POLLIN→DONT_WAIT drain으로 진행한다. |
| GIL을 유지하는 PyDLL submit | 기존 no-go. 다시 실험하지 않는다. |
| multipart를 단일 Core 호출로 합치기 | 기존 allocator corruption no-go. 이번 변경도 Core는 part별로 호출한다. |
| public wrapper/Future pool, 중앙 round-robin, 사전 tuple, Received pool, POLLIN+POLLCOMPLETION 혼합 | 기존 no-go. 다시 실험하지 않는다. |
| shield 제거 | cancellation 뒤 native operation과 private Future를 유지하는 기존 계약 테스트와 대조했다. 단순 삭제는 채택하지 않는다. |
| 미완료 상한·timeout·client 수 조작 | 배제. 고정 조건을 유지한다. |

러너는 `threading.Thread`와 자기 private asyncio loop를 유지한다. 다른 scheduler·thread로
진행을 넘기지 않는다(정책 §1.1.5). 요청 timeout, active/drain budget, 미완료 상한 64,
part 수와 집계 조건은 바꾸지 않았다. 기존 `asyncio.sleep(0)`의 continuation 양보를 유지하며
새 sleep은 추가하지 않았다. `loop._ready` 읽기는 CPython private loop 구현에 의존하므로
감독자가 검토할 유지보수 경계다. zlink의 내부 API를 러너에 추가한 것은 아니다.

### 최종 before / binding only / after

같은 시점에 각 회차를 **before → binding only → after** 순으로 세 번 반복했다.
before는 작업 시작 시 러너 snapshot과 원래 `_reply_payload`를 사용하고, binding only는
그 러너에 최종 binding만 적용했다. Core binary·입력·기한·상한은 동일하다.
진행 marker가 있는 `/tmp/zlink-python-reqrep/run_benchmarks.sh --pattern ...` 아래서
실행해 다른 perf의 idle 검사에도 보이게 했다. 출력은 `paired-{1,2,3}-{baseline,library,after}.log`다.

| 적용 | run 1 | run 2 | run 3 | 중앙값 ops/s | before 대비 |
|---|---:|---:|---:|---:|---:|
| before | 17,207 | 17,632.5 | 18,069 | **17,632.5** | — |
| binding only | 19,229.5 | 18,895.5 | 19,357 | **19,229.5** | **+9.1%** |
| binding + runner | 24,763.5 | 25,956 | 25,419 | **25,419** | **+44.2%** |

러너 수정의 추가 효과는 binding only 대비 +32.2%다. 이를 binding 효과에 합산하지 않는다.
초기 표준 러너 예비 측정은 reply만 적용 22,511.5 / 22,785.5 / 22,694,
러너 수정 뒤 24,527 / 24,220 / 24,185,
RoutingId 수정 뒤 24,633.5 / 25,247 / 25,383.5였다.
GIL 경합 때문에 실행 간 차이가 있으므로 최종 효과는 교차 측정으로 보고한다.

규칙 수: C 확장 사용 시 제출 part loop 소유자 **2개(C send/request, Python reply) → 1개**,
RoutingId bytes 검증의 길이 규칙 **2곳 → 1곳**. native part의 소비·close 규칙은 기존 것을 재사용한다.

## 4. 빌드·검증 상태

`setup.py build_ext --inplace --force`는 기존 bundled runtime이 0.17.0뿐이라
`Core runtime payload is missing`으로 실패했다. 기존 bundled 파일은 교체하지 않았다.
대신 시스템 Python의 `python3-config --includes/--extension-suffix`와 고정 0.17.1 헤더·lib로
**binding C 확장만** `gcc -shared -fPIC -O3 -pthread` 빌드했다.
Core 빌드·venv 생성·pytest 설치는 하지 않았다. `/proc/self/maps`에서 실제로 로드된 Core가
고정 prefix의 `libzlink.so.0.17.1` 하나뿐임을 확인했다.

```bash
source /tmp/zlink-python-reqrep/env.sh
gcc -shared -fPIC -O3 -pthread $(python3-config --includes) \
  -I "$ZLINK_CORE_PACKAGE_PREFIX/include" \
  bindings/python/src/zlink/_native/_zlink_native.c \
  -L "$ZLINK_CORE_PACKAGE_PREFIX/lib" -lzlink \
  -Wl,-rpath,'$ORIGIN/../native/linux-x86_64' \
  -o "bindings/python/src/zlink/_native/_zlink_native$(python3-config --extension-suffix)"
python3 -m unittest discover -s bindings/python/tests -p test_reply_native_unittest.py -v
```

| 검증 | 결과 |
|---|---|
| 새 unittest 7개 | 최종 7/7 통과. 받은 part·Message·mutable buffer 수명, 0/64/65536 B, 1/2/9-part, RID 길이와 비연속 memoryview, token owner/재사용 오류, async 64개와 취소 뒤 늦은 reply 검증 |
| 초기 5회 반복 | 4회 통과 후 5회째 async 자동 completion 경로 hang. 아래 별도 항목 참조 |
| 스택 계측을 켠 같은 7개 테스트 30회 | 210/210 통과. 앞선 간헐 실패가 해결됐다는 뜻은 아님 |
| single smoke | PAIR, DEALER_ROUTER, PUBSUB, DEALER_ROUTER_REQREP, ROUTER_ROUTER_REQREP × tcp/inproc × 64/65536 B, duration 1, 20/20 complete |
| multi smoke | DEALER_DEALER, DEALER_ROUTER_SENDSEND, PUBSUB, DEALER_ROUTER_REQREP × tcp × 1024/65536 B, clients 8, 명시 I/O threads 1, duration 1, 8/8 complete |
| 공개 API diff | `contracts/`, `__init__.py` 변경 0줄. 변경한 Python runtime의 public method/function signature AST 차이 0개 |
| diff whitespace | 요청 범위 `git diff --check` 통과 |

표준 smoke 출력은 `single-smoke.log`, `multi-smoke.log`이며, 테스트 출력은
`unit-final.log`, `unit-repeat.log`, `async-full-repeat.log`다. 기존 pytest 기반 전체 suite는
실행하지 않았다. smoke 값은 처리량 판정에 사용하지 않는다.

### 남은 실패: 자동 completion owner

public poller를 등록하지 않은 async request 테스트에서 65개 reply를 서버가 모두 보낸 뒤
일부 awaitable이 끝나지 않는 간헐 실패를 발견했다. stack에서는 서버 executor thread가
이미 작업을 끝내고 대기하고, native completion thread가
`routed_async.py:748`의 poller wait, asyncio thread가 selector wait에 있다.
이는 관측 위치이며, 아직 소실된 completion/continuation의 정확한 원인 위치는 확정하지 않았다.

원래 Python `_reply_payload`를 적용한 대조에서도 12회 통과 뒤 같은 hang을 재현했다
(`async-baseline-repeat.log`). 추가로 **작업 전 C 소스로 만든 확장 `baseline.so`와 원래
reply 구현**으로도 18회 통과 뒤 19회째 실패했다(`async-original-native.log`). 따라서
이번 reply C 경로에서 새로 생긴 실패로 보지 않는다. 테스트는 무한 대기 대신 기존 2초
요청 기한과 같은 bounded assertion에서 `TimeoutError`로 실패하도록 했다. expectation을
완화하거나 retry로 통과시키지 않았다. 최초 멈춘 테스트 process는 상태를 보존한 뒤 종료했다.
gdb attach는 호스트 ptrace 제한으로 실패하여 이후 재현부터 faulthandler 스택을 보존했다.

이 경로는 single의 public completion poller와 다르고, 이번 성능 병목 수정이 소유한 모듈도
아니다. 자동 completion owner 결함은 별도 B 진단 대상으로 남긴다. 해당 runtime 코드는
변경하지 않았다.

## 5. 감독자 검토

1. **10만 목표 달성으로 판정하면 안 된다.** 최종은 25,419 ops/s다. Node와 Python이 같은
   interpreter thread 조건이라는 가정은 성립하지 않는다. 현재 결과는 GIL·인터프리터 비용의
   규명과 부분 개선이며, 모든 가능한 binding 최적화를 소진했다는 결론은 아니다.
2. 자동 completion 경로의 기존 간헐 실패는 별도 원인 규명이 필요하다. 새 테스트에 재현을
   남겼으며, 이를 숨긴 채 전체 계약 검증 통과로 기록해서는 안 된다.
3. 러너의 CPython private ready queue 참조와 기존 Python multi I/O 기본값/정책 불일치는
   별도 검토할 항목이다. multi 파일·정책 문서를 이번 작업에서 고치지 않았다.

변경 파일은 `src/zlink/_native/hotpath.h`, `_runtime/sockets/socket_base_impl.py`,
`_runtime/handles/native_support.py`, `perf/single/perf_single_reqrep.py`,
`tests/test_reply_native_unittest.py`와 이 기록이다. 앞의 세 개가 binding 라이브러리 변경이다.

- 소유 계층: binding의 payload 변환·part 제출 경계. Core admission·재시도·token 판단은 그대로다.
- spec 근거: `bindings/doc/spec/python/README.ko.md:108-125`(ownership·동기 reply·typed error),
  고정 Core `include/zlink/socket/api.h:273-282`(part 소비·FINAL의 reply token 소비),
  러너는 `PERF_SINGLE_TEST_POLICY.md` §1.1.1~§1.1.5와 최적화 가이드 §2.3~§2.4.
- 교차언어 대조: Node의 native Message reply와 worker 격리를 확인했다. Python에는 GIL 공유와
  별도 ctypes reply loop가 있었고, 그 중 중복 제출 구현과 러너 비용을 줄였다.
- 변경 분류: **B — 기존 binding 비용·러너 결함 수정**. C/D 우회·spec 변경 없음.
