# Python binding hot-path pass 2

구현과 기능 gate를 완료했다. 최종 after는 20/20셀 complete이며 모든 셀이 지정 before의 3-run 평균보다 높다. **C 대비 평균 60%와 전체 경로의 Python 호출 한 자리 수 목표는 미달이다. EXIT:2.**

진행 로그의 3분 간격도 10개 구간에서 지키지 못했다(최대 492초). 기록은 실제 작성 시각을 유지했으며 소급 작성하지 않았다. 벤치·프로파일의 load 제한은 모두 지켰다.

## 처리량

TCP, clients 100, duration 5s, runs 1, 64/256/1024/4096/65536B, 공식 2-part 러너를 사용했다. Before는 main 작업 트리의 22:17:27~22:26:08 Python 보고서, C는 지정된 `*_p1python-r3q4.txt` 보고서다. 두 역사적 보고서의 `RESULT`는 중앙값이므로 **각 run 표의 처리량 세 값을 산술평균**했다. 표시 정밀도는 0.001 K 단위다. 아래 패턴 비율과 개선율은 크기별 비율 다섯 개의 산술평균이다. 동일 시각 A/B 재측정이나 통계적 유의성 검증은 하지 않았다.

| 패턴 | Before/C | After/C | 크기별 처리량 개선율 평균 |
|---|---:|---:|---:|
| DEALER_DEALER | 9.02% | 16.13% | +62.86% |
| DEALER_ROUTER_REQREP | 14.90% | 17.71% | +17.84% |
| ROUTER_ROUTER_REQREP | 16.46% | 20.05% | +24.44% |
| PUBSUB | 28.67% | 31.77% | +15.07% |

REQREP는 ops/s, 나머지는 msg/s다.

| 패턴 | B | Before 3-run 평균 | After | C 3-run 평균 | After/C |
|---|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 21,795.3 | 33,583.8 | 997,482.3 | 3.37% |
| DEALER_DEALER | 256 | 22,185.7 | 34,363.8 | 922,849.3 | 3.72% |
| DEALER_DEALER | 1024 | 22,061.0 | 34,428.0 | 842,100.3 | 4.09% |
| DEALER_DEALER | 4096 | 21,829.0 | 35,210.0 | 323,869.3 | 10.87% |
| DEALER_DEALER | 65536 | 18,889.3 | 35,508.6 | 60,590.0 | 58.60% |
| DEALER_ROUTER_REQREP | 64 | 14,606.0 | 17,040.0 | 206,552.3 | 8.25% |
| DEALER_ROUTER_REQREP | 256 | 14,325.0 | 16,695.2 | 177,026.0 | 9.43% |
| DEALER_ROUTER_REQREP | 1024 | 13,973.0 | 16,514.4 | 161,632.0 | 10.22% |
| DEALER_ROUTER_REQREP | 4096 | 13,632.7 | 16,060.0 | 138,731.0 | 11.58% |
| DEALER_ROUTER_REQREP | 65536 | 9,540.0 | 11,447.8 | 23,338.7 | 49.05% |
| ROUTER_ROUTER_REQREP | 64 | 13,504.3 | 17,240.0 | 172,081.3 | 10.02% |
| ROUTER_ROUTER_REQREP | 256 | 13,433.3 | 16,860.0 | 141,472.7 | 11.92% |
| ROUTER_ROUTER_REQREP | 1024 | 13,250.7 | 16,559.2 | 128,364.7 | 12.90% |
| ROUTER_ROUTER_REQREP | 4096 | 13,073.3 | 16,478.8 | 115,092.0 | 14.32% |
| ROUTER_ROUTER_REQREP | 65536 | 9,269.0 | 10,940.0 | 21,411.3 | 51.09% |
| PUBSUB | 64 | 157,540.0 | 196,244.6 | 785,530.0 | 24.98% |
| PUBSUB | 256 | 159,584.3 | 192,566.0 | 802,793.0 | 23.99% |
| PUBSUB | 1024 | 153,529.7 | 187,315.0 | 883,852.3 | 21.19% |
| PUBSUB | 4096 | 135,815.0 | 144,029.0 | 643,339.0 | 22.39% |
| PUBSUB | 65536 | 45,561.3 | 46,505.2 | 70,159.3 | 66.29% |

최종 자료: [공식 after report](reports/python-pass2-after-final.txt), [원본 이름의 report](reports/perf_python_multi_linux_20260905_234507.txt), [20셀 비교 및 원본 경로](reports/python-pass2-comparison.json). 구현 중간 측정도 [artifact 목록](reports/python-pass2-artifacts.json)에 보존했다. 최종 수치에서 좋은 run만 선택하지 않았다.

## Python 호출 분해

cProfile의 Python 함수 호출이며 C built-in은 제외한다. pass 1의 약 79회는 DD의 관측 wrapper 4.02회를 포함한 값이다. Wrapper를 제외한 pass 1은 74.67회다. 분모는 DD DATA submit/수신 건수, REQREP request 시작/서버 reply 건수다. STOP·setup·teardown 비용은 분자에 남으므로 평균에 소량 포함된다. 원본 pass 1 프로파일과 현재 최종 코드의 64B 공식 러너를 비교했다. 프로파일 결과는 처리량 판정에 섞지 않았다.

| 역할 | 전체 Python before→after | Binding before→after | Runner before→after | Stdlib 등 before→after | ctypes before→after |
|---|---:|---:|---:|---:|---:|
| dealer_dealer_client | 74.67→36.60 | 35.19→9.15 | 6.06→6.05 | 33.43→21.40 | 4.02→0.02 |
| dealer_dealer_server | 41.43→27.31 | 25.12→13.11 | 10.55→9.34 | 5.76→4.85 | 0.89→0.59 |
| dealer_router_reqrep_client | 154.05→99.18 | 74.30→33.34 | 9.08→9.09 | 70.67→56.75 | 11.03→3.05 |
| dealer_router_reqrep_server | 56.51→45.26 | 47.24→36.56 | 4.55→4.25 | 4.73→4.45 | 2.14→2.06 |

DD의 정상 즉시 성공 경로는 binding Python 호출 9개이며, 실측 평균은 9.15회다. 러너·stdlib를 합한 전체 호출은 한 자리 수가 아니다. REQREP client도 binding 33.34회가 남는다. 러너 파일은 변경하지 않았으며 runner 호출 평균의 차이는 readiness·drain 빈도 및 실행당 부대 비용 차이를 포함한다.

함수별 전체 분해는 [call census](reports/python-pass2-call-census.json)에 file/line·호출 수·메시지당 호출 수·자체 시간을 기록했다. DD before의 주요 binding 함수는 `lib` 4.02, `_handle` 2.01, `settled`·`context`·`_init_msg_from_buffer`·`_clone_native_msg` 각각 약 2회, `_materialize_native_parts`·`clone_payload`·`_attempt_send`·`_submit_parts`·`wait_async` 각각 약 1회다. 최종 정상 SEND는 `send`, builder `__init__`, `messages`, `submit`, `_payload_or_raise`, `submit_send`, `wait_async`, `_submit_parts`, `_handle`이 각각 1회 남는다.

## 구현 및 변경 파일

새 native packet/registry를 별도로 만드는 대안과 기존 Python entry·ctypes message 저장소의 실행만 C로 옮기는 대안을 비교했다. 기존 저장소와 owner를 유지하는 후자를 선택했다. Native 메서드는 같은 private Python 클래스에 설치되며 별도 상태 복사본을 만들지 않는다. Extension-free 경로도 유지했다.

| 파일 | 최종 변경 |
|---|---|
| `bindings/python/src/zlink/_native/hotpath.h` (신규) | SEND/REQUEST 시작, 기존 entry 초기화·snapshot clone·성공 정리·상태 조회, 수신 wrapper 구성, completion drain/capture/join/Future 전달, Message view/close를 C로 구현 |
| `bindings/python/src/zlink/_native/_zlink_native.c` | 기존 확장에 private entry point 연결, Python 3.9용 `Py_NewRef` 호환 처리 |
| `bindings/python/setup.py` | 신규 header를 extension dependency에 포함해 변경 시 재빌드 및 sdist 포함 보장 |
| `bindings/python/src/zlink/_runtime/messaging/routed_async.py` | 얇은 native 시작 wrapper, 기존 owner에 C 메서드 연결, SEND/REQUEST snapshot clone·해제 공통화 |
| `bindings/python/src/zlink/_runtime/messaging/native_parts.py` | 공통 materialization native 경계 |
| `bindings/python/src/zlink/_runtime/messaging/message_materializer.py` | 새 Received wrapper 구성 및 Message data/close native 연결 |
| `bindings/python/src/zlink/_runtime/sockets/socket_base.py`, `socket_base_impl.py` | recv/subscribe를 native 수신→wrapper 구성 한 호출로 연결 |
| `bindings/python/tests/test_message_storage_bridge.py`, `test_completion_contract.py` | 혼합/다수 part, caller ownership, snapshot, completion clone 수명, writable view/cache, cancellation 및 process-control 예외 검증 |

제출은 `start_send()`/`start_request()` 한 C 진입에서 materialization·DONTWAIT submit·토큰 등록을 수행한다. Native `zlink_send_part`/`zlink_request_part` 호출은 part별로 유지한다. 신규 제출 경로는 initialized header를 raw-copy하거나 재배치하지 않고 기존 ctypes 저장소를 pin한다. 기각된 contiguous multipart `prepared_parts` 전송 경로를 사용하지 않는다.

수신은 `recv_into()`/`router_recv_into()`/`subscribe_into()` 한 C 진입에서 기존 native recv와 새 Message wrapper 구성을 수행한다. Completion은 `_drain()` 한 C 진입에서 NO_DATA까지 처리하고 기존 entry의 capture/join과 Future 완료로 전달한다. Completion recv/close의 기존 ctypes FFI seam과 publish·registry 후처리의 일부 Python 메서드는 남는다. 전체 hot path가 Python을 전혀 거치지 않는 구현은 아니다.

## Ownership·오류·GIL

- Caller buffer/Message는 독립 native staging으로 제출한다. 재제출 snapshot은 native refcount clone이며 제출 실패가 caller Message를 소비하지 않는다. 최초 payload 복사와 retained clone의 의미는 유지했다.
- 받은 part wrapper는 매번 새로 만든다. Received.data는 Python bytes snapshot, Message.data는 기존 writable ctypes-backed view와 `(ptr, size, view)` cache를 유지한다. Public wrapper/Future/payload pool을 만들지 않았다.
- `SubmitError`/`RequestError`/`RecvError`/`ConfigError`/`CloseError`의 result와 native errno를 유지한다. NO_DATA만 False이며 실제 실패를 숨기지 않는다. Cancellation은 caller wait만 끝내고 late completion은 기존 owner가 정리한다.
- SEND 토큰 등록·lookup·재제출은 기존 owner lock과 map을 사용한다. REQUEST provisional 등록과 publish/capture 합류를 유지한다. 새 generation·retry budget·timer·thread·poller·spin·in-flight 제한은 없다.
- Submit은 **part마다** `Py_BEGIN_ALLOW_THREADS`/`Py_END_ALLOW_THREADS`로 기존 ctypes의 GIL 경계를 유지한다. Multipart 전체로 GIL 해제를 넓혔을 때 동시성 테스트가 실패하여 이 경계에서 수정했다. Blocking recv는 기존 C의 SaveThread/RestoreThread, blocking poller/wait는 기존 ctypes의 GIL 해제를 유지한다. GIL을 유지한 PyDLL 대체는 없다.
- Detached cleanup은 기존처럼 Exception만 처리하며 KeyboardInterrupt 같은 BaseException은 전파한다. Typed Request clone 실패는 기존 INTERNAL_ERROR/EIO 경계를 따른다.

**수정 전/후 규칙 수:** SEND/REQUEST snapshot clone·해제의 클래스별 구현 2→1 공통 메서드(각 실행 backend 내부), completion registry/drain owner 1→1, 신규 retry·wake 결정 규칙 0개. Immutable attribute 이름을 재사용할 뿐 별도 runtime 상태를 복제하지 않는다.

소유 계층: Python binding은 staging·Python 객체 수명·coroutine 완료를 소유하고, Core는 admission·WRITABLE·terminal 결과를 결정한다.
근거 spec: `bindings/doc/spec/python/README.ko.md`의 소유권과 수명·송수신과 no-data·Error·Pull completion, `core/doc/spec/core/socket/README.ko.md:911` Part send와 pending admission 및 `core/include/zlink/socket/api.h`.
교차언어 대조: `bindings/node/native/src/addon_core.cc:2113`의 native submit/ID 0 즉시 성공과 `bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp:179`의 part별 제출·`:244`의 WRITABLE correlation을 확인했다. Python은 기존 entry/Future/Condition을 유지하면서 Python/ctypes 실행 비용을 줄여야 하는 구조적 차이가 있다.
변경 분류: **B — 기존 binding hot-path 비용 결함**. 새로운 public API나 Core 정책은 추가하지 않았다.

## 검증과 실행 제약

- `python setup.py build_ext --inplace`: 성공, compiler warning 없음. Setuptools의 기존 license-table deprecation 안내는 별개다.
- 공식 `PYTHON_EXECUTABLE=... bindings/python/tests/run_tests.sh`: **216 passed + 4 subtests, samples 7/7**.
- 관련 storage/ownership/completion/request-WRITABLE tests: **80 passed × 5회**, 실패 없음.
- `git diff --check` 및 신규 header whitespace 점검: 통과. 공개 contracts·package root·러너 diff 없음. Public Python 메서드 signature는 변경하지 않았다.
- 수정은 `bindings/python/**`에 한정했다. Detached HEAD `fa7136cf1f9864e4029c96bbb5ea0a62c089d7c5`를 유지했다. 기존 `core/build`, `core/build-dev` untracked symlink는 그대로이며 Core build/clean, branch 전환, commit/push/reset/checkout/stash를 실행하지 않았다.
- Payload에는 요청된 0.17.0 symlink 세 개만 있다. Main Core와 측정용 복사본의 SHA-256은 모두 `d4b95b10ce3f96315740de20d2ea4e1d1d40f3dffd7a50de5530d26dff9a3f00`이다.
- 새 worktree source mtime 때문에 러너가 source-tree runtime을 stale로 판정했다. Main binary를 `bindings/python/build/perf-runtime/`으로 그대로 복사하고 기존 explicit runtime 옵션으로 측정했다. Core 파일이나 timestamp, 러너 검사는 수정하지 않았다. 지정 C/Python before report의 runtime hash와도 일치한다.
- 최종 after의 load 범위 **0.125–1.6704**, 최종 cProfile **0.7861–0.8550**. 전체 pass 2 계측 중 관측 최대는 **2.7832**다. 시작과 실행 중 load를 2초 간격으로 검사한 기록은 `reports/python-pass2-*-load.json`에 있다. 분석 자체는 benchmark/profile 실행과 구분했다.
- 진행 로그: [python-perf-pass2-progress.md](python-perf-pass2-progress.md). **3분 간격 미준수 구간 10회, 최대 492초**이며 [artifact audit](reports/python-pass2-artifacts.json)에 원 시각을 기록했다. 실행 중 git 금지 작업·Core build/clean은 없었고, 추가 agent를 사용하지 않았다.

## 남은 미달과 spec gap

기능 테스트의 남은 실패는 없다. C 대비 평균 60%는 모든 패턴에서 미달이며, 전체 Python 호출 한 자리 수도 미달이다. DD는 binding 정상 경로 9회까지 줄었으나 runner/asyncio 비용이 남고, REQREP에는 publish·registry·await 및 runner 비용이 남는다. 무할당이나 완전한 Python 실행 제거를 달성했다고 주장하지 않는다.

새 public API/ownership/error spec gap은 없다. 기존 Python spec의 exact dependency `0.16.0`(`README.ko.md:170`) 및 모든 send/request의 선행 provisional 등록 문구(`:174`)와 사용자 지정 Core 0.17 guide §2.1의 SEND 토큰 후 등록 규칙 사이 불일치는 pass 1에서 이어진다. 이번에는 기존 runtime 의미를 유지했고 보호된 spec을 수정하지 않았다.

완료 시각: 2026-09-05T23:53:57.512997+09:00

EXIT:2
