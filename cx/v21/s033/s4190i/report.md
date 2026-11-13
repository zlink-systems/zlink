# #4190 Integration 보고

**결과**
- 6개 runner에서 공통 규칙으로 gRPC × `request-backpressure`를 제외했고, Kotlin 보조 runner는 두 셀 모두 `request-serial @1024`만 실행한다.
- Java와 Node의 `peak_in_flight`를 stream별 최댓값의 합으로 맞췄다. .NET, C, C++도 기존부터 합산한다. README 한국어·영어 §5.2에 같은 뜻이 이미 있어 수정하지 않았다.
- 제품 코드와 benchagg 판단은 수정하지 않았다. 커밋 `9d0846684a` (`bench/4190-submit-threads`), push하지 않았다.

**바꾼 것 (전 → 후)**
- 셀 제외: `runner_common.sh:73-76`에는 skip 판정이 없었음 → 공통 `bench_skip_cell` `:78-81`. 호출 지점은 C `run_local.sh:61→61-62`, C++ `:88→88-89`, .NET `:55→55-56`, Java `:63→63-64`, Kotlin `:54→54-55`, Node `:61→61-62`.
- Kotlin 기본 셀: `java/run_local_kotlin.sh:7-8` `request-backpressure @1024` → `request-serial @1024`.
- Java peak: `BenchDrivers.java:699-702`의 owner 공유 계수/전역 최대 → stream 자체 계수 `:694-698`, terminal freeze에서 합산 `:818-840`. 테스트 `ActiveBoundaryTest.java:14-24` 추가.
- Node peak: `bench-core.js:54,67`의 전역 최대 → stream별 계수 `:54-55,64-70`, 합계 getter `:142-143`; 호출은 stream id를 전달한다. 테스트 `bench-core.test.js:45-54` 추가.
- 기존 합산 확인: .NET `Program.cs:782,792`, C `bench_zlink_client.cpp:375`, C++ `bench_cpp_client.cpp:412,419`; README 정의는 `README.ko.md:314`, `README.en.md:337`.
- Kotlin 기본값 테스트 기대 수정: `tools/tests/test_runner_common.py:63` backpressure → serial.

**검증**
- `python3 -m unittest discover -s framework/bench/grpc/tools/tests`: 134/134 통과 (`tools-tests-final.log`).
- Java `./gradlew --no-daemon --max-workers=1 :client:check :shared:check :kotlin-client:compileKotlin`: 성공 (`java-check.log`).
- Node `npm test`: 18/18 통과 (`node-tests-final.log`).
- .NET `BoundaryTests`, `WireTests`는 VSTest 프로젝트가 아니며 실행형 하네스로 검증: `dotnet run --project BoundaryTests/WithGrpcBench.BoundaryTests.csproj`의 네 패턴 PASS, `dotnet run --project WireTests/WithGrpcBench.WireTests.csproj` 29B wire PASS (`dotnet-boundary-run-final.log`, `dotnet-wire-run.log`).
- `bash -n` 7개 shell script와 `git diff --check` 통과 (`shell-check.log`). dry plan은 모든 runner에서 gRPC backpressure skip, Kotlin serial-only 확인 (`cell-plans.log`). 측정은 실행하지 않았다.
- C/C++ ctest 현지 재실행은 지정 wrapper가 외부 build/measurement lock을 22분 이상 기다려 중단했다. 이 변경은 두 언어의 runner shell만 건드렸고, 같은 병합 소스의 기존 ctest는 C 3/3·C++ 6/6 통과다 (`c-tests-merged-branch.log`, `cpp-tests-merged-branch.log`; 원본 `/root/jobs/fx/cx/v21/s033/s4190c/unit-{c,cpp}-final.log`).
- Java, .NET, Node 및 C 임시 build/install 산출물은 제거했다. C++ configure는 시작되지 않아 build output이 없다.

리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견. 수정·이관 항목 없음.

**남은 문제**
- C/C++ ctest는 이번 worktree에서 재실행하지 못했고, 위의 병합 branch 실행 결과를 근거로 기록했다.
