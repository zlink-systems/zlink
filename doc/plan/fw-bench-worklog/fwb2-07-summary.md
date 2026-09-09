# fwb2-07 — C++ server-driven bench 구현 보고

## 결과와 범위

`main`의 C++ bench를 구현별 source A / target B / HTTP trigger 모델로 변경했다.
`zlink-framework-cpp`에 RouteMesh의 typed protobuf request/send 경로를 추가했다.
네 패턴과 payload 1024·4096 입력을 지원하며, 요청된 smoke만 수행한다. 3-run 측정,
commit과 push는 수행하지 않았다. 규격·계획·집계기·다른 언어 bench·제품 runtime은 수정하지 않았다.

작업 시작 HEAD는 `4c2f2688d2`였다. 작업 중 다른 작업의 커밋으로 HEAD가
`a368b9f51d`로 진행했으며 브랜치는 계속 `main`이었다. 기존 untracked `fwb2-02-summary.md`,
`fwb2-03-summary.md`와 작업 도중 추가된 .NET runtime/test 변경 세 파일은 사용자 작업으로 보존했다.
임시 build·검증 원본은 `/tmp/zlink-astra-fwb2-07/`에 두었다. 티켓 시스템 자체 기록은
저장소의 `.artifacts/perf-queue/{done,log}/`에 남는다.

## 프로세스 구성

| 구현 | A / 사용 모듈 | A trigger / stats | B / 사용 모듈 | B endpoint / stats |
|---|---|---|---|---|
| `grpc-cpp` | `bench_cpp_client`, 시스템 grpc++ async unary stub + 공통 Framework HTTP host | 5280 / 5281 | `bench_cpp_grpc_server`, 기본 synchronous `ServerBuilder` | 5282 / 5283 |
| `zlink-cpp` | `bench_cpp_client`, C++ binding ROUTER + coroutine/poller + 공통 HTTP host | 5285 / 5286 | `bench_cpp_zlink_server`, request echo / command count ROUTER 분리 | request 5287, command 5288 / stats 5289 |
| `zlink-framework-cpp` | `bench_cpp_client`, `route_client_t`의 typed channel 호출 + 공통 HTTP host | 5292 / 5293 | `bench_cpp_framework_server`, RouteMesh typed echo/count handler + HTTP host | 5294 / 5295 |

runner는 5280–5299 전체 LISTEN preflight → B → A route readiness → warmup → warmup 수신
settle → B reset → active → bounded settle → JSON 병합·count 대조 → A/B 종료를 수행한다.
매 셀 새 프로세스 쌍을 사용한다. shell에는 부하 생성 코드가 없다.

A는 HTTP listener 두 개가 실제 응답한 뒤 outbound transport를 시작한다. 이 머신의
`ip_local_port_range=1024 65535`에서 outbound socket이 아직 열지 않은 고정 HTTP 포트를
차지하는 문제가 재현되어, Java의 같은 원인 수정과 대조했다. Framework A는 같은 프로세스 안에
HTTP용 app과 channel용 app을 두며 두 app의 lifecycle은 공통 `stats_http_server_t`가 소유한다.
Framework B는 HTTP와 channel을 app 하나에서 호스팅한다. Object role은 A/B 모두 `None`,
fixed RID와 manual peer 연결을 사용한다(공통 MeshNode 규격 §3.3·§4).

## 변경 파일과 diff 요지

| 파일 (`framework/bench/grpc/cpp/` 기준) | 변경 |
|---|---|
| `CMakeLists.txt` | 시스템 gRPC 유지, binding 0.17.6 / Core 0.17.5 package prefix, 저장소 Framework CMake target 참조, 새 target B와 protobuf codec 링크, 버전 metadata |
| `client/bench_cpp_client.cpp` | multi-cell CLI client를 단일 셀 source A로 변경. trigger 중복 acknowledgement·phase·stats, 세 driver, active 경계 표본과 최종 count 분리, raw send stream마다 실행 기회 보장 |
| `common/bench_stats_server.hpp` | 경량 socket HTTP responder를 Framework public HTTP hosting으로 교체. 공통 lifecycle와 callback, target role/count/header 검증 |
| `grpc/bench_grpc_cpp_server.cpp` | B 포트 적용. signal handler는 stop flag만 설정하고 main thread가 gRPC Shutdown 수행 |
| `zlink/bench_zlink_cpp_server.cpp` | B 포트 적용, 두 part/body 오류 집계, context shutdown으로 수신 thread 종료 |
| `framework/bench_framework_cpp_server.cpp` | 새 RouteMesh typed protobuf echo/send handler. 공용 codec 확장 사용 |
| `run_local.sh` | 셀별 A/B 기동·HTTP trigger·30초 settle·원본 병합·count 검증·종료와 고정 포트 preflight |
| `measure_span.sh` | 새 runner의 matrix 반복 wrapper. 기존 환경 변수 유지, 이번 작업에서는 실행하지 않음 |
| 이 보고서 | 검증·문서 반영용 표·결함과 재검증 명령 |

수정 전/후 규칙 수(셀 수명·오염 처리만 계산): **3 → 2**.
수정 전에는 runner의 공유 서버 수명, client의 multi-cell 수명, 다음 셀 contamination 전파를
따로 관리했다. 수정 후에는 runner가 셀별 프로세스 수명, A가 phase 수명을 소유하며,
프로세스 재사용과 다음 셀 contamination map을 제거했다. transport별 trigger·settle 복제는 없다.

두 대안 중 기존 multi-cell client를 HTTP로 감싸는 방식은 공유 서버 오염 상태를 유지해야 하므로
채택하지 않았다. 셀별 프로세스와 공통 phase 상태 관리로 수렴했다. HTTP와 channel을 같은 app에
두는 A 구성도 시도했지만, Framework의 channel 시작이 HTTP bind보다 앞서므로 실제 포트 충돌
수정에는 두 host를 같은 공통 lifecycle 구현으로 순차 시작하는 구성을 사용했다.

## 검증

최초 configure, Framework library build, bench 전체 build와 후속 변경의 focused rebuild는
각각 rc=0이었다. 모든 빌드는 시작 시 load average <10을 검사하고 한 번에 하나,
`--parallel 2`로 수행했다. `ldd`에서 Core가
`/home/hep7/.cache/zlink/core/0.17.5/linux-x64/lib/libzlink.so.0`이고 gRPC/protobuf는 시스템
라이브러리임을 확인했다.

모든 최종 smoke는 `--duration-seconds 2`, warmup 5초/10구간으로 수행했다.
아래 throughput은 **기능 검증용 RESULT 원본값**이며 3-run 성능 판정값이 아니다.

| 셀 | 티켓 | rc | RESULT throughput (ops/s 또는 msg/s) | 완료 / B 수신 | A 오류 / B 오류 / abandoned | peak in-flight |
|---|---|---:|---:|---:|---:|---:|
| `grpc-cpp request-serial@1024` | T1 | 0 | `13830.315` | 27661 / 27661 | 0 / 0 / 0 | 1 |
| `zlink-cpp request-serial@1024` | T2 | 0 | `7604.962` | 15211 / 15211 | 0 / 0 / 0 | 1 |
| `zlink-cpp send-saturation@1024` | T2 | 0 | `380492.048` | 760985 / 760985 | 0 / 0 / 0 | 1 |
| `zlink-framework-cpp request-serial@1024` | T3 | 0 | `554.925` | 1111 / 1111 | 0 / 0 / 0 | 1 |
| `zlink-framework-cpp request-window@1024` | T3 | 0 | `1051.500` | 2203 / 2203 | 0 / 0 / 0 | 100 |
| `zlink-framework-cpp send-saturation@1024` | T3 | 0 | `1101.972` | 8918 / 8918 | 0 / 0 / 0 | 8 |

- T1: `2-1788950157-40035-astra-fwb2-07-cpp_final_source_admission_and_trigger_s`
- T2: `2-1788950012-34346-astra-fwb2-07-cpp_raw_stream_fairness_final_smoke_2s`
- T3: `2-1788949551-13731-astra-fwb2-07-cpp_framework_RouteMesh_client_smoke_2s`

원본 위치: T1 `smoke-trigger-final/`, T2 `smoke-raw-streams/`, T3
`smoke-framework-route/` (모두 `/tmp/zlink-astra-fwb2-07/` 아래).
각 셀은 source RESULT 9줄, role·trigger 8필드·streams·target_stats를 가지며,
`submitted = completed + errors + abandoned`, request의
`completed ≤ received ≤ completed + errors + abandoned`, 오류 없는 send의 최종 완료/수신
동일성 대조를 통과했다. warmup과 active의 중복 trigger acknowledgement 동일성 및
필수 필드 누락 요청의 HTTP 400도 T1에서 확인했다.

| 검증 | 결과 |
|---|---|
| 최초 CMake configure / 최종 전체 bench build | rc=0 / rc=0 |
| `bash -n run_local.sh measure_span.sh` | rc=0 |
| `git diff --check` | rc=0 |
| 6개 셀 JSON 및 A/B count 검사 | rc=0 |
| 집계기 `--lang cpp --format spec4 --payload-sizes 1024` | rc=0, 세 패턴 표 출력; send 판정 한계는 아래 BLOCKERS 참조 |

`spec4.txt`, `spec4-error.txt`, `cpp-configure.log`, `cpp-build-final.log`,
`ticket-*.log`도 임시 root에 보존했다. 초기 시도에서 발견한 awk 내장 변수명 충돌(rc=2),
HTTP 포트 선점·gRPC 종료 교착, Object role 설정과 RouteMesh client 선택 문제(rc=1)는
수정 뒤 해당 셀을 재검증했다. 최초 집계 호출(rc=2)은 target 진단 snapshot을 독립 셀로
오인한 것으로, snapshot container를 사용하도록 runner를 수정했다. T3의 진단 stats
파일은 같은 container로 감쌌고, B 응답 필드/값과 `results.json`은 바꾸지 않았다.
T1/T2는 수정된 runner가 직접 이 형식으로 생성했다.


## 언어별 문서에 들어갈 표와 값 (계획 §5의 절 1–6)

### 1. 비교 대상

| 구현 | request API | send API | 모듈 |
|---|---|---|---|
| `grpc-cpp` | `PrepareAsyncEcho` / CompletionQueue | `PrepareAsyncCommand`, Empty reply | system `libgrpc++` |
| `zlink-cpp` | ROUTER `request(peer).message(...).async()` | ROUTER `send(peer).message(...).async()` | packaged `zlink::cpp` |
| `zlink-framework-cpp` | `route_client_t::request_to_channel(...).async<BenchPayload>()` | `route_client_t::send_to_channel(...).async()` | `zlink::framework`, `zlink::framework_codec_protobuf` |

Framework의 `channel_client_t`는 ClientServer 채널용이다. 이 벤치의 RouteMesh는
C++ exact interface `03-channel-messaging`의 `route_client_t`를 사용한다.

### 2. 실행 방법

runner는 빌드하지 않는다. 아래 재검증 명령의 build를 먼저 완료하고 티켓으로 실행한다.

| 입력 | 기본값 | 의미 |
|---|---|---|
| `BUILD_DIR` | `cpp/build` | 사전에 빌드한 실행 파일 위치 |
| `--implementations` / `IMPLEMENTATIONS` | 세 구현 모두 | 구현 목록 |
| `--patterns` / `PATTERNS` | 네 패턴 모두 | 패턴 목록 |
| `--payload-sizes` / `PAYLOAD_SIZES` | `1024,4096` | body 크기 |
| `--duration-seconds` / `DURATION_SECONDS` | 5 | active 초 |
| `--warmup` / `WARMUP_SECONDS` / `WARMUP` | 5 | warmup 초 |
| `--warmup-segments` / `WARMUP_SEGMENTS` | 10 | warmup throughput 관측 구간 수 |
| `REQUEST_WINDOW`, `SEND_CONCURRENCY` | 100, 8 | 다른 값은 거부 |
| `REQUEST_TIMEOUT_MS` | 30000 | request timeout |
| `DRAIN_BOUND_MS` | 30000 | source drain + runner settle 상한 |
| `COMMAND_SETTLE_MS` | 200 | 수신·완료 count 안정 확인 구간 |
| `OUTPUT_DIR` / `--output-dir` | `log/cpp/<stamp>/<label>` | 결과 디렉터리 |
| `RUN_STAMP` | 현재 시각 | run 묶음 ID |
| `LOAD_GATE` | 2.0 | 기존 C++ runner의 측정 시작 부하 기준 |

### 3. 프로세스 구성

앞의 A/B/포트 표를 사용한다. A의 logical stream 구현은 다음과 같다.

| 패턴 | stream 수 | stream당 in-flight | 구현 |
|---|---:|---:|---|
| request-serial | 1 | 1 | 단일 application submit/completion thread |
| request-window | 1 | 100 | gRPC async call / raw coroutine / Framework task의 미완료 집합 |
| request-backpressure | 1 | 상한 없음 (`null`) | 제출마다 completion pump에 기회를 주며 application 상한 없이 계속 제출 |
| send-saturation | 8 | 1 | gRPC는 stream당 stub 하나, raw는 coroutine slot, Framework는 task slot |

세 driver의 submit/completion 관측은 application thread 하나에서 수행한다.
Framework task의 완료 관측은 public `await_ready()` polling이며, 사용한 CPU도 숨기지 않고
`submit_thread_cores`에 포함한다. transport completion을 직접 drain하거나 두 번째 poller를
설치하지 않는다. Framework의 codec·reply routing은 Framework가 소유한다.

### 4. 언어별로 다르게 둔 값

| 항목 | 값 |
|---|---|
| warmup | 5초, 10개 구간. trigger 원본의 `warmup`은 **5000 ms** |
| active | 기본 5초; 이번 smoke는 2초 |
| gRPC B | synchronous `ServerBuilder` 기본값, insecure loopback, TLS/compression/tuning 없음 |
| gRPC A | channel 하나, logical stream당 unary stub 하나, application thread의 CompletionQueue |
| compiler | GNU C++ 13.3.0, C++20, Release / `-O3` |
| gRPC / protobuf | 시스템 1.51.1 / 3.21.12 |
| C++ binding / Core | 0.17.6 / release 0.17.5 |
| Framework | 저장소 0.11.0, public CMake source target |
| 포화 계측기 / 상한 | `CLOCK_THREAD_CPUTIME_ID`, `submit_thread_cores` / 1 |
| latency 표본 상한 | A 200000개, B 2000000개 (기존 값) |

### 5. 결과 위치

기본 run root는 `framework/bench/grpc/log/cpp/<stamp>/<label>/`이다.
`runner.log`, `report.txt`, `load-gates.txt`와 셀별 디렉터리를 둔다. 셀 디렉터리에
`source.log`, `target.log`, `warmup-target-stats.json`, `target-stats.json`, `results.json`이 있다.
두 stats 파일은 `{ "snapshot": <B 응답 원문 객체> }`로 보관한다. stats 응답의 `role=target`을
root에 두면 집계기가 trigger 없는 독립 셀로 오인하므로, 진단 snapshot과 셀 원본을 구분한다.
`results.json`은 `with-grpc-cell-v1`, source `role`, trigger 필수 8필드, `streams`,
settle 뒤 `target_stats`를 포함한다. source의 `completed_at_close`와 최종 `completed`,
`server_received_at_close`와 최종 target 수신을 구분한다.

### 6. 알려진 제약

- 구현상 unsupported 분기는 없다. 이번 검증은 지정된 1024-byte smoke에 한정하며 4096-byte,
  request-backpressure, 3-run 재현성과 성능 우열은 검증하지 않았다.
- Framework A는 public task를 polling하므로 포화 여부는 실제 `submit_thread_cores`와 함께 읽는다.
- HTTP host의 여러 listen 포트에 같은 route가 노출된다. 규격상 trigger URL은 지정 trigger
  포트로 기록하고 runner도 그 URL만 사용한다.
- 아래 집계기 문제 때문에 send 성능 판정에는 추가 수정·재검증이 필요하다.

## Runtime 결함과 BLOCKERS

- **Framework HTTP bind 실패 전달**: `framework/languages/cpp/framework/src/runtime/http/http_listener.cpp:149`
  에서 bind 실패를 throw하고, `:524`가 실행하는 listener thread는 그 예외를 host 시작 결과로
  전달하지 않는다. raw A의 실제 포트 충돌에서 프로세스 abort로 관측했다. HTTP bind는
  `app.is_ready()` 이전에 끝난다는 보장도 없다(`:515–526`). 제품 코드는 변경하지 않았다.
  벤치는 이미 요구된 HTTP 응답 readiness로 listener 시작을 확인한다.
- **집계기 send 경계값 덮어쓰기**: `framework/bench/grpc/tools/benchagg/readers.py:606`은 source의
  `server_received_at_close`를 settle 뒤 `target_stats.received`로 덮고, `:615`는 send throughput을
  그 값/trigger duration으로 재계산한다. C++ source의 active 경계값은 원본에 보존했다.
  집계기는 수정 금지 범위이므로 **send 성능 판정의 BLOCKER**로 남긴다.
  Framework send smoke에서 active 경계는 2204건, settle 뒤는 8918건이었다. 원본 RESULT는
  1101.972 msg/s인데 집계기는 4459.000 msg/s로 바꾼다. source와 B의 최종 count는 일치했고
  오류/abandoned는 0이며 drain은 5546.379 ms였다. drain 상한 30초 안의 backlog 관측으로,
  이것만으로 새 제품 결함을 단정하지 않는다.
- gRPC signal handler의 `Shutdown()` 교착은 기존 **bench** 결함으로 수정했다.
  Framework Object role과 RouteMesh client 선택도 bench 설정/호출 수정이며 제품 결함으로 분류하지 않는다.

runtime 소유권: 제품 runtime 변경 없음. A의 phase·부하·계측은 bench, transport·routing·codec은 기존 소유 모듈.
규격 근거: bench §3·§4·§6·§9·§10, MeshNode §3.3·§4, C++ exact channel interface.
교차언어 대조: .NET RouteMesh protobuf request/send와 Java listener 예약·count 불변식을 대조했다.
변경 분류: bench 계약 적응(A) 및 기존 bench 결함 수정(B). 제품 runtime 수정 승인 대상 변경은 없다.

## 분담 결과 검토

runner 담당의 awk 변수명·계측 경계·count 대조 제안을 원본과 직접 대조해 반영했다.
HTTP 담당의 `app.run()` signal 설치, listener thread 예외 전달 위치를 직접 확인했다.
RouteMesh listen 누락 및 trigger exact field 검증 제안은 공개 인터페이스와 대조해 반영했다.
`channel_client_t` 사용이 적합하다는 초기 리뷰 의견은 기각했다. C++ exact interface와
`ChannelEgressRouting/Server/role.cpp:544–551`은 RouteMesh 호출에 `route_client_t`를 사용한다.

## 재검증 명령

```bash
# 매 build 시작 전 load average <10, 한 번에 한 build만 실행한다.
python3 -c 'import os; assert os.getloadavg()[0] < 10'
cmake -S bindings/cpp -B /tmp/zlink-astra-fwb2-07/binding -G Ninja \
  -DCMAKE_PREFIX_PATH=/home/hep7/.cache/zlink/core/0.17.5/linux-x64 \
  -DCMAKE_INSTALL_PREFIX=/tmp/zlink-astra-fwb2-07/packages/install/zlink-cpp/0.17.6 \
  -DZLINK_CPP_BUILD_TESTS=OFF
cmake --build /tmp/zlink-astra-fwb2-07/binding --parallel 2
cmake --install /tmp/zlink-astra-fwb2-07/binding

cmake -S framework/bench/grpc/cpp -B /tmp/zlink-astra-fwb2-07/cpp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT=/tmp/zlink-astra-fwb2-07/packages \
  -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=/home/hep7/.cache/zlink/core/0.17.5/linux-x64
cmake --build /tmp/zlink-astra-fwb2-07/cpp --parallel 2

bash scripts/perf/perf-ticket.sh submit -p 2 -o astra-fwb2-07 -d 'cpp serial smoke 2s' -- \
  env BUILD_DIR=/tmp/zlink-astra-fwb2-07/cpp OUTPUT_DIR=/tmp/zlink-astra-fwb2-07/recheck-serial \
  bash framework/bench/grpc/cpp/run_local.sh recheck-serial \
  --implementations grpc-cpp,zlink-cpp,zlink-framework-cpp \
  --patterns request-serial --payload-sizes 1024 --duration-seconds 2

bash scripts/perf/perf-ticket.sh submit -p 2 -o astra-fwb2-07 -d 'cpp raw send smoke 2s' -- \
  env BUILD_DIR=/tmp/zlink-astra-fwb2-07/cpp OUTPUT_DIR=/tmp/zlink-astra-fwb2-07/recheck-send \
  bash framework/bench/grpc/cpp/run_local.sh recheck-send \
  --implementations zlink-cpp --patterns send-saturation --payload-sizes 1024 --duration-seconds 2

bash scripts/perf/perf-ticket.sh submit -p 2 -o astra-fwb2-07 -d 'cpp framework window/send smoke 2s' -- \
  env BUILD_DIR=/tmp/zlink-astra-fwb2-07/cpp OUTPUT_DIR=/tmp/zlink-astra-fwb2-07/recheck-framework \
  bash framework/bench/grpc/cpp/run_local.sh recheck-framework \
  --implementations zlink-framework-cpp --patterns request-window,send-saturation \
  --payload-sizes 1024 --duration-seconds 2

python3 framework/bench/grpc/tools/bench_aggregate.py --lang cpp --format spec4 \
  --payload-sizes 1024 \
  --runs-glob '/tmp/zlink-astra-fwb2-07/smoke-trigger-final/*' \
  --runs-glob '/tmp/zlink-astra-fwb2-07/smoke-raw-streams/*' \
  --runs-glob '/tmp/zlink-astra-fwb2-07/smoke-framework-route/*'

bash -n framework/bench/grpc/cpp/run_local.sh framework/bench/grpc/cpp/measure_span.sh
git diff --check
```
