# C·Rust·Go·Python·C++·.NET multi 성능 측정 계획

## 기준과 셀

측정 소스는 이 문서가 병합된 뒤의 main이다. `run-all.sh`는 실행 당일 `MEASURE_REPO`·`MEASURE_BRANCH`가 가리키는 HEAD를 별도 디렉터리에 복제하고 그 SHA를 결과에 기록한다. 계획을 처음 쓸 때 기준은 `94d335507c`였다.

C 기준선의 `*.run.log`에 실린 실행 커밋은 `c067ffa254`다. 18개 `*.meta.txt`의 `head=64635d5fd2`는 `run-one.sh`가 `/root/zlink`에서 수집한 값이다. `cbase3-r1`~`r3`의 파일명과 `steps*.txt`에서 전체 회차를 식별했다. 각 로그의 `META,runs,1`은 그 호출 안의 반복 횟수다. `meta.txt`는 원래 명령줄과 전체 환경 변수를 기록하지 않는다. 아래 옵션은 `run.log`의 Effective Options, 콘솔의 빌드·Core 정보, `meta.txt`의 주파수 정보에서 복원했다. 규칙의 출처는 [C 기준 결과](/mnt/nas/work/logs/studio/c-baseline-1128-final/cbase3-r1-DEALER_DEALER.run.log:1), [C 기준 실행 메타데이터](/mnt/nas/work/logs/studio/c-baseline-1128-final/cbase3-r1-DEALER_DEALER.meta.txt:1), [run-one.sh](/root/perf-runs/run-one.sh:22)다.

TCP에서 100 clients, active 5초, paced latency 5초, 서버·클라이언트 I/O 스레드 각 4개로 맞춘다. 파트 수는 C 러너 기본값과 바이트 예산 실험에서 확인한 2를 명시한다. Core는 릴리스 1.26.0이다. 지정 캐시의 `libzlink.so.1.26.0`은 기준선 로그 경로의 라이브러리와 SHA-256이 일치한다. CPU는 `run-one.sh`가 2.0 GHz로 고정하고 검증한다. 모니터 HWM은 4,096,000 B, auto-HWM 프로필은 `balanced`다. 연결 동시성은 128, 연결·서버 준비 제한은 각 10,000 ms, 송신·수신 제한은 각 200 ms, 서버 종료 제한은 5,000 ms다. transport·패턴·회차 간 설정값은 각 3,000 ms이며 포트 0으로 자동 할당한다. [C 기본 파트 수](/root/wt/measplan/bindings/c/perf/run_benchmarks_multi.sh:62) · [바이트 예산 실험 조건](/root/cxjobs/c-burst/report.md:23)

아래 표는 세 회차 각각에서 확인한 동일한 36셀이다. 마지막 열의 백분율은 각 지표의 유효한 세 회차 값에서 `(최댓값 / 최솟값 − 1) × 100`으로 산출했다. 처리량, 평균 지연, p99 중 하나의 변동이 10%를 초과하거나 지연 무효가 있으면 반복한다. `—`는 1회 대상이다.

| 패턴 | transport | 크기 (B) | C 회차 간 변동 또는 무효 |
|---|---|---|---|
| DEALER_DEALER | tcp | 64 | p99 32.5% |
| DEALER_DEALER | tcp | 256 | p99 10.8% |
| DEALER_DEALER | tcp | 1024 | p99 12.1% |
| DEALER_DEALER | tcp | 4096 | — |
| DEALER_DEALER | tcp | 65536 | 처리량 13.9%, 평균 11.3% |
| DEALER_DEALER | tcp | 131072 | 평균 16.8%, p99 75.9% |
| DEALER_ROUTER_SENDSEND | tcp | 64 | 평균 190.4%, p99 407.9% |
| DEALER_ROUTER_SENDSEND | tcp | 256 | 처리량 17.3%, 평균 230.3%, p99 94.6% |
| DEALER_ROUTER_SENDSEND | tcp | 1024 | 평균 303.0%, p99 347.9% |
| DEALER_ROUTER_SENDSEND | tcp | 4096 | — |
| DEALER_ROUTER_SENDSEND | tcp | 65536 | p99 24.3% |
| DEALER_ROUTER_SENDSEND | tcp | 131072 | 평균 18.3%, p99 143.8% |
| ROUTER_ROUTER_SENDSEND | tcp | 64 | 평균 64.0%, p99 105.4% |
| ROUTER_ROUTER_SENDSEND | tcp | 256 | 평균 207.2%, p99 243.9% |
| ROUTER_ROUTER_SENDSEND | tcp | 1024 | 평균 521.0%, p99 399.1% |
| ROUTER_ROUTER_SENDSEND | tcp | 4096 | 처리량 33.6%, 평균 98.0%, p99 168.1% |
| ROUTER_ROUTER_SENDSEND | tcp | 65536 | — |
| ROUTER_ROUTER_SENDSEND | tcp | 131072 | 평균 161.4%, p99 885.9% |
| DEALER_ROUTER_REQREP | tcp | 64 | — |
| DEALER_ROUTER_REQREP | tcp | 256 | 평균 20.3% |
| DEALER_ROUTER_REQREP | tcp | 1024 | 평균 19.1%, p99 13.3% |
| DEALER_ROUTER_REQREP | tcp | 4096 | 평균 15.1% |
| DEALER_ROUTER_REQREP | tcp | 65536 | p99 19.3% |
| DEALER_ROUTER_REQREP | tcp | 131072 | 평균 33.9%, p99 117.8% |
| ROUTER_ROUTER_REQREP | tcp | 64 | 평균 15.3% |
| ROUTER_ROUTER_REQREP | tcp | 256 | 평균 40.9%, p99 23.9% |
| ROUTER_ROUTER_REQREP | tcp | 1024 | — |
| ROUTER_ROUTER_REQREP | tcp | 4096 | 평균 13.5% |
| ROUTER_ROUTER_REQREP | tcp | 65536 | 평균 39.7%, p99 471.9% |
| ROUTER_ROUTER_REQREP | tcp | 131072 | 평균 26.1%, p99 59.7% |
| PUBSUB | tcp | 64 | 평균 367.7%, p99 199.5% |
| PUBSUB | tcp | 256 | 평균 무효 1회, 평균 172.6%, p99 무효 1회, p99 38.7% |
| PUBSUB | tcp | 1024 | 평균 422.7%, p99 147.9% |
| PUBSUB | tcp | 4096 | — |
| PUBSUB | tcp | 65536 | — |
| PUBSUB | tcp | 131072 | 평균 17.5%, p99 13.4% |

## 순서와 반복

1. 지정 브랜치의 소스를 `/root/cxjobs/measplan/measplan-<UTC>-<PID>/source`에 복제한다. C → Rust → Go → Python → C++ → .NET 순으로 빌드한다. 모든 빌드는 `nice -n 10`, 최대 8개 작업으로 한 번에 하나씩 수행한다.
2. C 36셀을 한 번 측정한다. 위 표에서 표시한 29셀을 각각 두 번 더 측정하고 셀별 중앙값을 새 C 기준으로 사용한다. 이후 Rust → Go → Python → C++ → .NET 순으로 각 언어의 초기 측정과 추가 회차를 연속으로 완료한다.
3. 언어별 첫 36셀의 처리량, 평균 지연, p99를 같은 셀의 새 C 기준과 비교한다. 어느 하나라도 C의 90~110% 범위를 벗어나거나 평균 또는 p99가 한쪽에서 무효이면 해당 셀을 두 번 더 측정한다. 이 셀과 역사적 변동 29셀의 합집합을 사용한다. 90%와 110%는 허용 범위에 포함한다. 유효한 지연만 중앙값에 반영하고 유효 회차 수와 무효 사유를 함께 기록한다.
4. 각 호출은 한 패턴·한 transport·한 회차로 제한하고 해당 크기 목록을 한 번에 전달한다. `run-one.sh`가 호출별 배타 잠금을 얻는다. 잠금을 요청하기 직전과 잠금 안에서 러너를 시작하기 직전에 1분 load < 12와 MemAvailable ≥ 10 GiB를 확인한다. 조건 위반이나 러너 또는 결과 검증 실패 시 중단한다. 후속 실행은 새 이름을 사용하고 각 결과는 `/root/perf-runs/measplan-<UTC>-<PID>-<언어>-r<회차>-<패턴>/`에 보존한다. [잠금과 주파수 검증](/root/perf-runs/run-one.sh:5) · [실행 순서와 잠금 안 검사](/root/cxjobs/measplan/run-all.sh:23)

기본 `run-all.sh` 또는 `run-all.sh --dry-run`은 계획된 명령과 파일 검사 결과를 출력한다. 최초 반복 대상의 호출까지 출력하며 언어 결과가 있어야 정해지는 추가 셀은 `table.py repeat` 명령으로 표시한다. 향후 실제 수행은 별도 `--execute` 모드로 호출한다.

## 빌드와 진입점

빌드와 러너는 같은 격리 소스 경로를 사용한다. 공통 Core 경로는 `/root/.cache/zlink/core/1.26.0/linux-x64`이고, `ZLINK_CORE_SOURCE=release`, `ZLINK_CORE_RELEASE_VERSION=1.26.0`, `ZLINK_CORE_PACKAGE_PREFIX`를 전달한다. 아래 경로는 격리 소스 루트 기준이다.

| 언어 | 빌드와 산출물 | multi 러너 | 언어별 환경과 입력 |
|---|---|---|---|
| C | CMake Release, `ENABLE_LTO=OFF`, `ZLINK_C_BUILD_BENCHMARKS=ON`, `ZLINK_C_CORE_BUILD_DIR`로 지정 Core 연결. `bindings/c/build-release-1.26.0/perf/comp_src_*_{server,client}` 12개 | `bindings/c/perf/run_benchmarks_multi.sh` | `--latency-duration 5 --monitor-hwm-bytes 4096000` |
| Rust | `cargo build --release --locked --jobs 8 --manifest-path bindings/rust/perf/multi/Cargo.toml`. `$CARGO_TARGET_DIR/release/perf_multi_*_{server,client}` 12개 | `bindings/rust/perf/run_benchmarks_multi.sh` | `PATH=/root/toolchains/cargo/bin:…`, `RUSTUP_HOME=/root/toolchains/rustup`, 캠페인 안 `CARGO_HOME`과 `CARGO_TARGET_DIR`, `ZLINK_RUST_NATIVE_DIR=$core/lib`, `PERF_MULTI_LATENCY_DURATION_SECONDS=5`, `--monitor-hwm-bytes 4096000` |
| Go | `go -C bindings/go/perf build -p 8 -o "$go_build/perf_multi" ./multi` | `bindings/go/perf/run_benchmarks_multi.sh` | `PATH=/root/toolchains/go/bin:…`, `CGO_ENABLED=1`, `CGO_CFLAGS=-I$core/include`, `CGO_LDFLAGS="-L$core/lib -Wl,-rpath,$core/lib"`, `ZLINK_GO_NATIVE_DIR=$core/lib`, `PERF_MULTI_LATENCY_DURATION_SECONDS=5`, `--build-dir "$go_build" --monitor-hwm-bytes 4096000` |
| Python | Core를 `bindings/python/src/zlink/native/linux-x86_64`에 복사하고 SONAME 링크를 만든 뒤 `/root/toolchains/venv/bin/python3 setup.py build_ext --inplace --force -j 8`. `src/zlink/_native/_zlink_native*.so` | `bindings/python/perf/run_benchmarks_multi.sh` | `PYTHON_EXECUTABLE=/root/toolchains/venv/bin/python3`, 복사본을 가리키는 `ZLINK_LIBRARY_PATH`, `PERF_MULTI_LATENCY_DURATION_SECONDS=5`, `--monitor-hwm 4096000` |
| C++ | CMake Release, `ENABLE_LTO=OFF`, `ZLINK_CPP_BUILD_BENCHMARKS=ON`, `ZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON`. `bindings/cpp/perf/multi/build/cpp_comp_src_*_{server,client}` 12개 | `bindings/cpp/perf/run_binding_multi.sh` | `ZLINK_CPP_CORE_BUILD_DIR`는 CMake에 전달한다. `--latency-duration 5 --monitor-hwm-bytes 4096000` |
| .NET | `dotnet build bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/Zlink.BindingBench.Multi.csproj -c Release -m:1`. 고정 `bin/Release/net8.0/Zlink.BindingBench.Multi.dll` | `bindings/dotnet/perf/multi/run_benchmarks.sh` | MSBuild 서버·공유 컴파일·노드 재사용을 끈다. `--latency-duration 5 --monitor-hwm-bytes 4096000` |

모든 언어에 `--reuse-build --pattern <패턴> --transports tcp --msg-sizes <크기 목록> --runs 1 --duration 5 --clients 100 --part-count 2 --io-threads 4`와 위 공통 옵션을 전달한다. `--output`과 `--results-dir`는 `run-one.sh`가 덧붙인다. [Rust 지연 환경 변수](/root/wt/measplan/bindings/rust/perf/run_benchmarks_multi.sh:805) · [Go 지연 환경 변수](/root/wt/measplan/bindings/go/perf/run_benchmarks_multi.sh:1202) · [Python CLI](/root/wt/measplan/bindings/python/perf/multi/run_benchmarks.py:264) · [Python 지연 환경 변수](/root/wt/measplan/bindings/python/perf/multi/perf_multi_measurement.py:35) · [.NET 빌드 레이아웃](/root/wt/measplan/bindings/dotnet/perf/multi/run_benchmarks.sh:17)

## 예상 시간

| 범위 | 셀 실행 수 | active 5초 | active 5초 + latency 5초 |
|---|---:|---:|---:|
| C 포함 6개 구현 초기 1회 | 36 × 6 = 216 | 18분 | 36분 |
| 역사적 변동 29셀을 각 2회 추가 | 29 × 6 × 2 = 348 | 29분 | 58분 |
| 계획된 기본 합계 | 564 | 47분 | 94분 |
| 나머지 7셀까지 언어 5개가 모두 재측정할 경우 | 564 + 7 × 5 × 2 = 634 | 52분 50초 | 105분 40초 |

위 표는 설정된 phase 시간의 합이다. 실제 벽시계 시간에는 빌드, 연결·종료, 잠금 대기, 주파수 검증, paced 준비 및 PUBSUB catch-up이 추가된다. 예정된 `run-one.sh` 호출은 108회다. 기존 C의 18호출은 `meta.txt`의 시작·종료 기준 총 25분 16초였으며, 같은 셀 수의 phase 합 18분보다 7분 16초 길었다. 모든 패턴에 이미 역사적 변동 셀이 있어 추가 판정은 해당 호출의 크기 목록만 늘린다. [PUBSUB catch-up 예산](/root/wt/measplan/doc/perf/PERF_MULTI_TEST_POLICY.md:1292)

## C active burst 바이트 예산

기존 실험의 관측 대상은 C의 DEALER_DEALER 및 두 SENDSEND다. TCP에서 64, 1024, 4096, 65536, 131072B의 15셀을 예산별로 2회 측정했다. 두 REQREP에도 실험 소스의 공통 바이트 판정이 적용됐지만 report의 측정표에는 값이 없다. PUBSUB와 다섯 언어에는 이 실험의 바이트 예산을 할당할 근거가 없다. 256B 역시 기존 실험표에 값이 없다. 아래는 해당 실험 자체의 main 대비 처리량 변화율이다. 본 계획의 새 C 기준과는 별개의 실험 데이터다. [적용 코드와 측정 조건](/root/cxjobs/c-burst/report.md:12) · [원본 수치](/root/cxjobs/c-burst/report.md:26)

| 패턴 | 크기 (B) | 32 KiB | 64 KiB | 256 KiB |
|---|---:|---:|---:|---:|
| DEALER_DEALER | 64 | -0.1% | -1.1% | +5.2% |
| DEALER_DEALER | 1024 | +0.6% | +1.5% | +2.6% |
| DEALER_DEALER | 4096 | -1.4% | +0.1% | -0.9% |
| DEALER_DEALER | 65536 | -36.6% | -38.6% | -11.8% |
| DEALER_DEALER | 131072 | -45.7% | -45.4% | -30.8% |
| DEALER_ROUTER_SENDSEND | 64 | +2.9% | +5.1% | +3.6% |
| DEALER_ROUTER_SENDSEND | 1024 | +4.4% | -0.8% | +2.0% |
| DEALER_ROUTER_SENDSEND | 4096 | +8.7% | +1.9% | +3.6% |
| DEALER_ROUTER_SENDSEND | 65536 | +44.6% | +45.4% | +0.8% |
| DEALER_ROUTER_SENDSEND | 131072 | +34.8% | +38.2% | +13.1% |
| ROUTER_ROUTER_SENDSEND | 64 | -0.2% | -1.7% | -4.7% |
| ROUTER_ROUTER_SENDSEND | 1024 | +1.7% | +0.5% | -0.7% |
| ROUTER_ROUTER_SENDSEND | 4096 | -3.4% | +1.1% | -9.0% |
| ROUTER_ROUTER_SENDSEND | 65536 | +14.9% | +19.8% | +2.0% |
| ROUTER_ROUTER_SENDSEND | 131072 | +41.2% | +37.9% | +17.3% |

빌드는 실험 소스가 `PERF_EXPERIMENT_BURST_BYTES`를 읽는 상태에서 CMake Release 및 `-DENABLE_LTO=OFF`로 별도 디렉터리를 사용했다. 각 `CMAKE_CXX_FLAGS` 값은 `-DPERF_EXPERIMENT_BURST_BYTES=32768`, `=65536`, `=262144`다. 이번 dry-run은 제공된 세 빌드의 CMake 캐시와 비교에 쓰인 서버·클라이언트 18개의 존재를 검증한다. 현재 브랜치 소스에는 이 실험 매크로가 없다. 예산 비교는 위의 보존 결과를 사용하며 새 언어 측정 캠페인에는 포함하지 않는다. [32 KiB 캐시](/root/cxjobs/c-burst/build-32k/CMakeCache.txt:39) · [64 KiB 캐시](/root/cxjobs/c-burst/build-64k/CMakeCache.txt:39) · [256 KiB 캐시](/root/cxjobs/c-burst/build-256k/CMakeCache.txt:39)

## 결과 표

`table.py compare --index <캠페인>/runs.tsv` 또는 `table.py compare C=<결과 디렉터리> Rust=<결과 디렉터리> …`로 표를 생성한다. 언어별 회차 수만큼 입력을 반복할 수 있다. `RESULT`에서 active 처리량과 paced 평균·p99 지연을 읽는다. 비율 100%는 새 C와 같은 값이다. BENCH_DIAG의 paced 목표, paced 수신, caught_up 및 늦은 송신 비율은 회차 순서대로 표시한다. Rust는 콘솔의 진단으로 보완한다. 진단 누락은 `—`, 실제 `caught_up=no`는 `no`로 표시한다.

| 언어 | 패턴 | transport | 크기 (B) | 회차 | 처리량 (/s) | 평균 (ms) | p99 (ms) | 처리량 / C | 평균 / C | p99 / C | paced 목표 (/s) | paced 수신 (/s) | caught_up | 늦은 송신 (%) | 유효 평균 지연 | 무효 사유 |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|---|---|---|
| C 또는 언어 | 패턴명 | tcp | 메시지 크기 | 1 또는 3 | 중앙값 | 중앙값 | 중앙값 | 비율 | 비율 | 비율 | 회차별 | 회차별 | 회차별 | 회차별 | 유효/전체 | 기록값 |

`table.py legacy /mnt/nas/work/logs/studio/c-baseline-1128-final`로 기준선 `table.md`의 1·2회 값을 재생성한다. 레거시 표의 `nan`은 기준선의 표시를 재현하며 새 표에는 무효 사유를 보존한다. [RESULT 형식](/root/wt/measplan/doc/perf/PERF_MULTI_TEST_POLICY.md:458) · [중앙값과 중복 행 정책](/root/wt/measplan/doc/perf/PERF_MULTI_TEST_POLICY.md:806)
