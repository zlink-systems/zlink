# Core 0.17.0 bindings 성능 측정 환경 (2026-09-07 재부트스트랩)

기록일은 2026-09-07이다. 측정 호스트가 교체되었고 `core/build`(Release+LTO) 트리도
존재하지 않았으므로, 이 manifest는 계획서 §1·§4·§6에 따라 Core artifact와 toolchain을
현재 호스트에서 다시 실측해 고정한다. 이 문서 시점까지 실행한 것은 smoke뿐이며 성능
기준값은 아직 만들지 않았다.

## 1. 버전 일치 확인 (계획서 §1)

| 파일 | 확인한 값 |
|------|-----------|
| `VERSION` | `LIBZLINK_VERSION_MAJOR=0`, `LIBZLINK_VERSION_MINOR=17`, `LIBZLINK_VERSION_PATCH=0`, `LIBZLINK_VERSION=0.17.0` |
| `core/CMakeLists.txt:11` | `project(zlink VERSION 0.17.0 LANGUAGES C CXX)` |
| `core/include/zlink.h:8-10` | `ZLINK_VERSION_MAJOR 0`, `ZLINK_VERSION_MINOR 17`, `ZLINK_VERSION_PATCH 0` |

세 곳 모두 0.17.0으로 일치한다.

## 2. Source와 Core runtime

| 항목 | 값 |
|------|----|
| source | `main` / `c39f50f6dc037285a0ffbf746f7282da487449d3` |
| 작업 트리 상태 | dirty. 단, 변경은 전부 `framework/**`의 무관한 잔여 변경이다(아래 목록). `core/**`, `bindings/**`, `doc/perf/**`에는 변경이 없다 |
| 무관한 잔여 변경 (수정·커밋·되돌림 모두 하지 않음) | `framework/doc/framework/common/bench/with-grpc-local.en.md`, `framework/doc/framework/common/bench/with-grpc-local.ko.md`, `framework/languages/cpp/bench/with-grpc/client/bench_cpp_client.cpp`, `framework/languages/node/bench/with-grpc/client/bench-core.js`, `framework/languages/node/bench/with-grpc/client/main.js`, 그리고 untracked `framework/languages/cpp/bench/with-grpc/log/smoke/`, `framework/languages/cpp/bench/with-grpc/log/smoke2/`, `framework/languages/node/bench/with-grpc/log/smoke/` |
| Core runtime (절대 경로) | `/home/hep7/project/zlink/core/build/lib/libzlink.so.0.17.0` |
| `readlink -f` | `/home/hep7/project/zlink/core/build/lib/libzlink.so.0.17.0` (`libzlink.so` → `libzlink.so.0` → `libzlink.so.0.17.0`) |
| `file` | `ELF 64-bit LSB shared object, x86-64, version 1 (SYSV), dynamically linked, BuildID[sha1]=af759a1c5532fb7100c6baede89144814200d798, not stripped` |
| ELF Build ID (`readelf -n`) | `af759a1c5532fb7100c6baede89144814200d798` |
| Core SHA-256 | `0af61ad39b5830fdb3f2f8538aed9f26bea70487bbb862df2df1a6f6023dfd72` |
| Core build 모드 | `CMAKE_BUILD_TYPE:STRING=Release`, `ENABLE_LTO:BOOL=ON`, `ZLINK_BUILD_TESTS:BOOL=OFF`, `CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG` (`core/build/CMakeCache.txt`) |
| Core provenance | GitHub `core/v0.17.0` release asset이 아직 없으므로 로컬 workspace artifact다. `c39f50f6dc037285a0ffbf746f7282da487449d3` 시점의 `core/`를 `scripts/build-core.sh release`로 새로 build했다 |
| 빌드 시각 | 2026-09-07 08:08 (KST) |
| report META 확인 | `META,core_source,local` / `META,core_version,0.17.0` / `META,core_runtime,/home/hep7/project/zlink/core/build/lib/libzlink.so.0.17.0` / `META,core_revision,c39f50f6dc037285a0ffbf746f7282da487449d3` / `META,core_dirty,0` |

이 캠페인 동안 Core는 이 하나의 artifact로 고정한다. 계획서 §5대로 측정 도중 `core/build`를
다시 build하지 않는다.

### 2.1 혼동 위험이 있는 다른 Core artifact (사용 금지)

`~/.cache/zlink/core/0.17.0/linux-x64/lib/libzlink.so.0.17.0` 이 존재한다. Build ID
`99d5e2a07d753facd8ba41a37a8582eb0d554980`, SHA-256
`602861c38b0012a5f5879515345c515fdd29138a4dc8df502af27da8ac90214a` 로 위 measurement
artifact와 **다른 파일**이며, 이전 캠페인에서 `core/build-dev`(RelWithDebInfo, LTO OFF)로
materialize한 local package다. `--core-version 0.17.0`(release 모드)을 쓰면 이 prefix가
선택되므로, 이번 캠페인에서는 `--core-version`을 사용하지 않고 `ZLINK_CORE_SOURCE=local`
(기본값)만 사용한다.

## 3. Host

| 항목 | 값 |
|------|----|
| 환경 | Linux x86_64, WSL2, Ubuntu 24.04.4 LTS |
| kernel | `6.6.87.2-microsoft-standard-WSL2` |
| CPU | Intel(R) Core(TM) Ultra 7 265K |
| 논리 CPU | 20 (`nproc`, `lscpu`) |
| 물리 구성 | 1 socket, 20 cores/socket, 1 thread/core |
| memory | 94.29 GiB (`MemTotal: 98874480 kB`), 측정 시 `MemAvailable: 91161924 kB` |
| CPU governor | WSL2에서 `/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` 미노출 |
| CPU pinning | 사용하지 않음(`--pin-cpu` 미사용) |
| `ulimit -n` | 1048576 |
| 측정 중 다른 고부하 작업 | 없음. smoke는 직렬로만 실행했다. C smoke 시 load_avg `1.28 1.02 0.69`, C++ smoke 시 `0.51 0.85 0.65` |
| 홈 디렉터리 | `/home/hep7` |

## 4. Compiler와 언어 runtime

| 항목 | 버전 / 경로 |
|------|-------------|
| GCC / G++ | Ubuntu GCC 13.3.0 (`gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`, `g++` 동일) |
| Clang / Clang++ | 설치되지 않음(`command not found`) |
| CMake | 3.28.3 |
| Ninja | 1.11.1 |
| Node | `v22.23.2` |
| .NET SDK | `8.0.130`, `/usr/lib/dotnet/sdk` |
| Java 기본 runtime/toolchain | Temurin `22.0.2+9` (`/usr/lib/jvm/temurin-22-jdk-amd64`), `javac 22.0.2` |
| Java perf runtime/toolchain | 별도 `~/.jdks` JDK 없음. `JAVA_HOME`은 unset이며 시스템 기본 Temurin 22.0.2+9를 사용한다 |
| Go | `go1.22.2 linux/amd64` |
| Cargo | `1.85.0 (d73d2caf9 2024-12-31)` |
| Rustc | `1.85.0 (4d91de4e4 2025-02-17)` |
| Python | `3.12.3` |
| pytest venv | 현재 호스트에 없음. `~/.cache/zlink`에는 `core/`, `native/`만 존재하며 `python-test-venv`는 아직 만들지 않았다 |
| C++ binding public version API | `zlink::version()` → `0.17.0` (`bindings/cpp/build/libzlink_cpp.a` + `core/build/lib/libzlink.so.0` 링크로 직접 확인) |

## 5. 사용한 명령

Core build:

```bash
scripts/build-core.sh release
```

C multi smoke:

```bash
PERF_FAIL_FAST=1 ZLINK_CORE_SOURCE=local \
bindings/c/perf/run_benchmarks_multi.sh \
  --pattern MULTI_DEALER_DEALER --transports tcp --msg-sizes 64 --duration 1 --runs 1
```

C++ multi smoke:

```bash
PERF_FAIL_FAST=1 ZLINK_CORE_SOURCE=local \
bindings/cpp/perf/run_benchmarks_multi.sh \
  --pattern MULTI_DEALER_DEALER --transports tcp --msg-sizes 64 --duration 1 --runs 1
```

두 runner 모두 `--pattern` 값의 `MULTI_` 접두사를 벗겨 내부 pattern 이름으로 정규화한다.
smoke는 계획서 §7.0·§7.1대로 완전히 직렬로 실행했고, 측정 조건(duration/HWM/timeout/client 수)은
어느 것도 완화하지 않았다.

## 6. Runner의 Core 해석 경로

두 runner 모두 `ZLINK_CORE_SOURCE=local`(기본값)에서 별도 package prefix 없이 workspace의
`core/build`를 직접 사용한다. `~/.cache/zlink/core` prefix를 materialize할 필요가 없었고,
수동 심볼릭 링크 등 임시 조치도 만들지 않았다.

- C: `bindings/c/perf/run_benchmarks_multi.sh`가 `DEFAULT_CORE_BUILD_DIR=<repo>/core/build`를
  사용하고, benchmark 실행 전에 각 binary의 `ldd`로 `libzlink.so.0` 해석 결과를 대조한 뒤
  `Verified benchmark Core runtime: /home/hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`을
  출력했다.
- C++: `bindings/cpp/perf/run_binding_multi.sh`가 같은 `core/build`를 사용하고
  `ZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON`을 강제한다. `bindings/cpp/build/CMakeCache.txt`에
  `ZLINK_CPP_CORE_BUILD_DIR:PATH=/home/hep7/project/zlink/core/build`,
  `_core_lib:FILEPATH=/home/hep7/project/zlink/core/build/lib/libzlink.so`가 기록되었고,
  `ldd bindings/cpp/perf/multi/build/cpp_comp_src_dealer_dealer_client`가
  `libzlink.so.0 => /home/hep7/project/zlink/core/build/lib/libzlink.so.0`을 보여 준다.

### 6.1 C++ 첫 smoke의 stale-check 경로 불일치 (기록)

C++ 첫 smoke(08:10:20)는 실행 시작 시점에
`Perf core build dir: /home/hep7/project/zlink/core/build-dev`를 출력했다. 이는
`ensure_core_runtime_not_stale()`가 **재configure 이전**의 `bindings/cpp/build/CMakeCache.txt`에
남아 있던 이전 캠페인의 `core/build-dev` 값을 읽었기 때문이다. 그 직후 runner가 기본값
`core/build`로 재configure하여 실제 링크는 `core/build`로 이루어졌고, report META의
`core_runtime`도 `core/build`였다. cache가 정정된 뒤 같은 조건으로 다시 실행한
smoke(08:11:21)는 시작 시점부터 `Perf core build dir: /home/hep7/project/zlink/core/build`를
출력했다. 이 manifest의 공식 C++ smoke는 재실행분(08:11:21)이다.

## 7. Smoke 결과

| 대상 | 조건 | report | status |
|------|------|--------|--------|
| C multi | `MULTI_DEALER_DEALER` / tcp / 64 B / duration 1 s / runs 1 / clients 100 / io threads 4·4 | `/home/hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_080912.txt` | `complete` (success 1, fail 0, expected 5 / actual 5 RESULT lines) |
| C++ multi | 위와 동일 | `/home/hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_081121.txt` | `complete` (success 1, fail 0) |

C++ 첫 실행분 `/home/hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_081020.txt`도
`status: complete`였으나 §6.1 사유로 참고 기록으로만 남긴다.

두 report 모두 `META,core_version,0.17.0`,
`META,core_runtime,/home/hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`,
`META,core_dirty,0`을 기록했다. C++ binding public version API도 `0.17.0`을 반환한다.

smoke는 실행 경로와 종료 상태 확인용이며(계획서 §7.2), 아래 수치는 기준값이 아니다.

| 대상 | Throughput | Bandwidth | Lat.Mean | Lat.P95 | Lat.P99 |
|------|-----------|-----------|----------|---------|---------|
| C | 1681.483 Kmsg/s | 107.615 MB/s | 0.074 ms | 0.219 ms | 1.867 ms |
| C++ | 1126.293 Kmsg/s | 72.083 MB/s | 0.082 ms | 0.202 ms | 2.114 ms |

## 8. Inventory gate 재확인 (계획서 §4)

C runner와 C++ runner의 `--help`, shell registry, `run_comparison.py` 상수를 계획서 §3·§4
및 `log/2026-09-05-environment.ko.md`의 표와 대조했다.

일치하는 항목:

| 항목 | C | C++ | 계획서/이전 manifest |
|------|---|-----|----------------------|
| Single pattern | `PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,DEALER_ROUTER_REQREP,ROUTER_ROUTER,ROUTER_ROUTER_REQREP` | 동일 | 일치 |
| Multi pattern (canonical) | `MULTI_DEALER_DEALER,MULTI_DEALER_ROUTER_SENDSEND,MULTI_DEALER_ROUTER_REQREP,MULTI_ROUTER_ROUTER_SENDSEND,MULTI_ROUTER_ROUTER_REQREP,MULTI_PUBSUB,MULTI_STREAM` | 동일 집합 | 일치 |
| Single 기본 msg size | `64,256,1024,65536,131072,262144` | 동일 | 계획서 §3.1과 일치 |
| Multi 기본 msg size | `64,256,1024,4096,65536,131072` | 동일 | 계획서 §3.2와 일치 |
| `MULTI_STREAM` 기본 msg size | `64,256,1024,65536` | 동일 | 계획서 §3.2와 일치 |
| Single 기본 transport | `tcp,tls,ws,wss,inproc,ipc` (Linux) | 동일 | 일치 |
| Multi 기본 transport | `tcp,tls,ws,wss` | 동일 | 일치 |
| 기본 client 수 | 100 | 100 | 일치 |
| 기본 io thread | server 4 / client 4 | 동일 | 일치 |
| 기본 duration | multi 5 s / single 5 s | 동일 | 일치 |
| sndtimeo / rcvtimeo | 200 ms / 200 ms | 동일 | 일치 |
| memory guard cap | smoke에서 발생하지 않음 | 발생하지 않음 | 일치 |

불일치로 확인한 항목(감독자 판정 필요):

1. `default_stream_clients` 보고 상수. `bindings/c/perf/run_comparison.py:3960`은 `100`,
   `bindings/cpp/perf/run_comparison.py:3921`은 `10000`이다. 실제 STREAM client 수는 두
   shell runner 모두 `EFFECTIVE_DEFAULT_STREAM_CLIENTS=100`으로 결정하므로 실행값은 같지만,
   report의 "Effective Options"에는 C가 `100`, C++이 `10000`으로 찍힌다. 계획서 §4의
   "C와 binding report에서 실제 client 수가 같은지 확인" 항목을 report만으로는 만족시킬 수
   없다. `MULTI_STREAM` paired 측정 전에 정리가 필요하다.
2. monitor HWM option과 환경 변수 이름. C는 `--monitor-hwm-bytes` /
   `PERF_MULTI_MONITOR_HWM_BYTES`, C++은 `--monitor-hwm` / `PERF_MULTI_MONITOR_HWM`이다.
   기본값은 둘 다 4096000으로 같지만, 한쪽 이름으로 override하면 다른 쪽에는 적용되지 않는다.
3. "Effective Options" key 이름. C `routed_echo_per_socket_payload` ↔ C++
   `routed_echo_borrow_payload`, C `monitor_hwm_bytes` ↔ C++ `monitor_hwm`.
4. paired smoke의 auto-HWM detail에서 **server DEALER socket의 적용된 HWM이 다르다.**
   같은 조건(tcp / 64 B / clients 100 / balanced profile)에서 client 행은 양쪽 모두
   `SNDHWM=RCVHWM=1048576`인데, server 행은 C가 `1048576`, C++이 `4096000`이다. 계획서 §6은
   서로 다른 effective HWM을 비교에 반영하도록 요구하므로, `MULTI_DEALER_DEALER` paired
   측정을 시작하기 전에 원인 규명이 필요하다.
5. `select_transports()` 구현 차이. C는 `STREAM_VARIANT_PATTERNS`와
   `CONTROL_PLANE_PATTERNS`를 모두 STREAM transport 집합으로 처리하지만 C++은
   `STREAM_VARIANT_PATTERNS`만 처리한다. 또한 `--transports`를 지정했을 때 C는 사용자 지정
   순서를, C++은 base 순서를 따른다. 현재 등록된 multi pattern 집합에는 control-plane
   pattern이 없어 이번 캠페인 범위에는 영향이 없다.
6. `--help`의 기본 pattern 나열 순서가 다르다(C: `...ROUTER_ROUTER_SENDSEND,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP...`,
   C++: `...DEALER_ROUTER_REQREP,ROUTER_ROUTER_SENDSEND,ROUTER_ROUTER_REQREP...`). 집합은
   같고 한 번에 하나의 pattern만 실행하므로 측정에는 영향이 없다.

## 9. 2026-09-05 manifest 대비 달라진 점

| 항목 | 2026-09-05 | 2026-09-07 |
|------|-----------|-----------|
| 호스트 | 12th Gen Intel Core i7-1260P, 논리 CPU 16 (1 socket / 8 core), memory 11.68 GiB, 홈 `/home/hep7hep7` | Intel Core Ultra 7 265K, 논리 CPU 20 (1 socket / 20 core / 1 thread per core), memory 94.29 GiB, 홈 `/home/hep7` |
| Core artifact | Build ID `1e93b62c49d42f4de000c415cfc1eb4faae94aab`, SHA-256 `a98cc793...61025`, provenance `29add0ac81` | Build ID `af759a1c5532fb7100c6baede89144814200d798`, SHA-256 `0af61ad3...dfd72`, provenance `c39f50f6dc037285a0ffbf746f7282da487449d3` |
| source commit | `87057e86542787fb1ef9c0e3d9a0d60ffc09fe4a` | `c39f50f6dc037285a0ffbf746f7282da487449d3` |
| Node | v24.19.0 | v22.23.2 |
| Cargo / Rustc | 1.75.0 | 1.85.0 |
| Java 기본 | OpenJDK 21.0.12 (perf는 `~/.jdks/jdk-22.0.2+9`) | 시스템 기본이 Temurin 22.0.2+9 하나뿐, 별도 perf JDK 없음 |
| pytest venv | `/home/hep7hep7/.cache/zlink/python-test-venv` (pytest 9.1.1) | 없음 (필요 시 새로 생성해야 하며, 생성 시 이 manifest에 추가한다) |
| GCC / .NET / Go / Python | 13.3.0 / 8.0.130 / go1.22.2 / 3.12.3 | 동일 |

계획서 §6은 Core release version/tag, package provenance, runtime, host boot, CPU governor,
client 수, toolchain 또는 성능 관련 환경 변수가 바뀌면 이전 C 결과와 새 binding 결과를
짝지어 판정하지 말라고 규정한다. 위 표대로 호스트와 Core artifact가 모두 교체되었으므로
**2026-09-05 manifest로 측정한 모든 값(`log/2026-09-05-*.ko.md`의 before/after 포함)은
이번 캠페인의 기준값이나 판정 근거로 사용할 수 없다.** 모든 paired 측정은 이 manifest의
조건으로 C부터 다시 시작한다.

## 10. 측정 규칙

- 동시에 perf process는 하나만 실행한다.
- `--pin-cpu`는 사용하지 않는다.
- 모든 C와 binding runner에 `ZLINK_CORE_SOURCE=local`을 명시한다. `--core-version`은
  사용하지 않는다(§2.1).
- Java perf는 시스템 기본 Temurin 22.0.2+9를 사용한다. 별도 `JAVA_HOME`은 지정하지 않는다.
- Python 검증용 venv는 아직 없다. 만들 때 경로와 pytest 버전을 이 manifest에 추가한다.
- paired C와 binding은 같은 session tag, pattern, transport, size, duration, runs, client 수와
  I/O thread 수로 순차 실행한다.
