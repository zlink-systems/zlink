# progress-measure-cppserver

- 15:06 시작. common-rules 확인 완료. with_stream README / cppserver stack / runner 조사 시작.
- 15:08 cppserver upstream 서브모듈류(.gitlinks 기반) 미존재 확인. asio/Catch2/cpp-optparse/CppBenchmark/CppCommon 및 중첩 의존성(fmt/HdrHistogram/zlib) 네트워크 클론 완료. 이제 cppserver 스택 사전 빌드 시도(로드 유발, idle 대기 전에 수행).
- 15:12 core release lib(0.17.3) 재빌드 시작(JOBS=4, lib-only). 완료 대기 중(백그라운드 알림 대기, 그 사이 다른 빌드 시작 안 함).
- 15:13 core/build/lib/libzlink.so.0.17.3 빌드 완료(release --lib-only, exit 0). cppserver 스택 빌드로 진행.
- 15:2x cppserver 스택 사전 빌드 완료. 필요했던 조치: upstream/modules 하위 asio/Catch2/cpp-optparse/CppBenchmark/CppCommon(및 CppCommon 안의 CppBenchmark 재귀, fmt/HdrHistogram/zlib) git clone, 그리고 CppCMakeScripts(cmake/ 헬퍼)를 cppserver 루트뿐 아니라 modules/CppCommon, modules/CppBenchmark, modules/CppCommon/modules/CppBenchmark 각각에 복사(각 CMakeLists.txt가 자기 cmake/ 하위 디렉터리를 CMAKE_MODULE_PATH로 잡음). 이후 build-stream 구성/빌드 정상 완료. 소스 미수정, .gitlinks 기반 외부 의존성만 채움.
- 이제 idle window 대기(ninja 0, 1분 load < 1.5 두 번 연속) 후 PERF_LOCK 하에서 4-스택 측정 실행.
- 15:2x with_stream 기존 빌드(zlink/asio/zmq/client 바이너리)는 core/build 라이브러리(0.17.3)를 동적 링크하므로 재빌드 불필요 확인, cppserver도 --reuse-build 조건(바이너리 존재) 충족. idle window 대기 시작(ninja==0 & 1분 load<1.5 연속 2회, 최대 40분).
- 15:37 idle window 대기 지속 중(background poll script, load 변동 심함: 1.0~4.0대 반복). 대기 스크립트가 조건 충족 시 자동 종료하도록 이미 실행 중; 완료되는 대로 PERF_LOCK 하에서 측정 실행 예정.
- 15:37 idle window 도달(ninja=0, load1<1.5 연속 2회: 15:36:00 load1=1.05, 15:37:02 load1=1.23). PERF_LOCK 하에서 4-스택 측정 시작.
- 15:37:40 PERF_LOCK 획득 시도했으나 다른 세션(다른 job, pid 3520607-계열)이 이미 동일 with_stream 벤치를 15:37:01부터 실행 중이어서 대기(flock 블로킹, 정상 동작). 그 job은 15:48:28 완료(zlink,asio,asio_pull,cppserver,cppserver_pull,zmq 6-스택). 완료 즉시 내 flock이 잠금을 획득하고 15:48경 본 측정 시작.
- 15:56 1차 측정 완료(results/20260908_154828). 그러나 절대 kops가 과거 idle 기준(예: 65536B zlink 34.7 kops) 대비 크게 낮음(10.71 kops) — 직전(15:37~15:48) 다른 세션의 with_stream 벤치가 PERF_LOCK을 먼저 잡고 실행한 잔여 부하(load avg 시작 6.96) 오염 의심. 진짜 idle에서 재측정 1회 시도(규칙상 필요시 2회 허용).
- 16:02 재측정 시도는 PERF_LOCK을 다른 job(all3)이 재선점해 대기만 지속(16:01:43까지도 잠김). 시간 상한 준수 위해 중단. 1차 측정 결과(오염 주의 명시)로 보고서 작성 완료: doc/plan/c016-worklog/measure-cppserver-summary.md
