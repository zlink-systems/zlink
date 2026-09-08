# gate-st 진행

- 2026-09-08 KST: 감독관이 main index에 staged한 ST-1/2/3 patch(6개 core 파일)를 확인했다. apply/pull은 지시대로 생략했고 staged 상태를 보존한다. brief·공통 규칙·ST-3 보고서·ST-2 리뷰를 읽었다.
- 다음: ninja 동시 빌드 제한을 확인한 뒤 `JOBS=4 scripts/build-core.sh dev`, 전체 ctest, 변경 suite 반복 검증을 실행한다.
- 2026-09-08 KST: dev configure는 성공했고 `JOBS=4` 빌드를 계속 수행 중이다. 시작 전·중 확인에서 `ninja` 동시 실행 수는 0이었다. 컴파일 오류는 아직 관측되지 않았다.
- 2026-09-08 KST: dev 빌드의 Core/test-core 객체 컴파일이 진행 중이며, 변경된 socket lifecycle/message 관련 객체도 오류 없이 컴파일 단계에 들어갔다. 빌드 완료 직후 전체 ctest를 1회 실행한다.
- 2026-09-08 08:37 KST: 2회차를 재개했다. staged ST-1/2/3 범위(수정 5개·신규 통합 테스트 1개)와 공개 인터페이스 diff 부재를 다시 확인했다. dev 빌드 시작 직전 `ninja=0`, available 메모리 10566 MiB로 메모리 규칙을 충족했다.
- 2026-09-08 08:40 KST: dev 빌드는 99%까지 진행했고 변경된 `socket_base_*`와 `test_stream_concurrent_pull_send` 컴파일·링크에 오류가 없었다. 기존 `unittest_monitor_ready_drain` 컴파일에서 GCC `-Wstringop-overflow` 경고 1건과 미지원 warning-option 주석이 있었으나 빌드를 중단시키지 않았다.
- 2026-09-08 08:44 KST: dev 빌드는 성공했다. 전체 ctest는 진행 중이며 89/211 기능 테스트가 통과했다. 기본 등록된 `hotpath_gate`는 기준보다 10.0~27.9% 빨라 보호 규칙상 FAIL을 냈다(기능 실패가 아니며 별도 PERF_LOCK 측정 대상으로 기록). 나머지 실패 유무를 끝까지 확인한다.
- 2026-09-08 08:45 KST: 전체 ctest가 종료했다. 기능 테스트 210/210 PASS(총 252.15 s)이며 `hotpath_gate`만 위의 기준 보호 FAIL이었다. 이제 변경 관련 suite 3회와 신규 STREAM 정체 회귀 테스트 until-fail:20을 실행한다.
- 2026-09-08 08:48 KST: 변경 suite의 `until-fail:3` 반복(98 CTest 항목)이 진행 중이다. 가장 긴 시작 항목 `test_wake_invariants` 3/3 및 socket-disconnect-progress·router reciprocal handover 반복은 모두 통과했다. 실패 관측 없음.
- 2026-09-08 08:51 KST: 변경 suite 98/98 PASS, 0 실패(341.55 s)로 3회 반복을 마쳤다. `test_stream_concurrent_pull_send`만 별도로 until-fail:20을 실행한다.
- 2026-09-08 08:52 KST: 신규 회귀 `test_stream_concurrent_pull_send`는 20/20 PASS(3.92 s)였다. staged/unstaged 공개 헤더·버전 스크립트 diff는 비어 있고 `zlink.h`·`zlink_enum.h`·`zlink_errno.h`의 C/C++/Go/Rust mirror 12/12가 일치했다. release 빌드 전 메모리 조건을 확인한다.
- 2026-09-08 08:54 KST: release 시작 검사에서 다른 job의 `ninja=1`을 확인했다. 강한 2회차 조건(반드시 0)을 즉시 재적용해 49/86 뒤 빌드를 중단했다(exit 130, 컴파일 오류 없음). 이 작업의 release 빌드는 시작으로 세지 않으며, 이후 `ninja=0`·available 6000 MiB 이상을 확인한 뒤 이어서 실행한다.
- 2026-09-08 08:57 KST: 60초 대기 뒤 `ninja=0`, available 10673 MiB를 확인해 release lib-only를 성공적으로 완료했다. 이어 `core/build-gate`의 Release+LTO `hotpath_bench`도 성공했다. 측정 직후 부하가 5.20이어서 현재 load<1.5 조건을 60초 간격으로 대기 중이며, 최신 1분 load는 1.79이다.
- 2026-09-08 08:58 KST: 유휴 조건(load 0.47/2.31/3.90, ninja 0, available 10628 MiB)에서 PERF_LOCK hotpath 5셀을 1회 완료했다. 전부 PASS: dealer-dealer 1.0300, dealer-router reqrep 1.0058, pair 1.0127, router-router tcp 1.0054, stream tcp 1.0155(reference 대비 ratio). 다음 with_stream·perf/c multi 측정 전에도 같은 조건을 확인한다.
- 2026-09-08 09:00 KST: PERF_LOCK 아래 local Release lib를 사용한 with_stream 1회가 완료했다(결과 `results/20260908_085903`, mismatch 0). zlink kops/s는 64/1024/65536 B에서 285.52/274.78/34.43, asio는 378.39/342.19/42.36, zmq는 332.49/304.11/23.16이었다. perf/c multi 시작 전에는 다른 job의 `ninja=1`과 load 3.30을 관측해 대기한다.
- 2026-09-08 09:03 KST: perf/c multi의 시작 보류를 계속한다. 두 번의 60초 확인에서 `ninja=1`이 유지됐고 load1은 3.92→4.15였다(available 8259→10186 MiB). 재실행 메모리 규칙상 `ninja=0` 전에는 측정하지 않는다.
- 2026-09-08 09:06 KST: 다른 두 job의 빌드가 겹쳐 `ninja=2`, load1 7.88까지 상승했다(available 9496 MiB). PERF_LOCK은 획득하지 않았고, 60초 폴링을 계속한다. 대기 상한은 perf/c 보류 시작부터 30분이다.
- 2026-09-08 09:10 KST: `ninja=0`, available 10715 MiB, load 0.91에서 PERF_LOCK perf/c multi 3셀을 완료했다. 1024 B tcp 처리량은 DR_REQREP 233.090, RR_SENDSEND 250.754, PUBSUB 1039.496 Kops/s이고 모두 성공했다. 요약 보고서 `gate-st-summary.md`를 작성했으며 staged Core patch는 그대로다.
