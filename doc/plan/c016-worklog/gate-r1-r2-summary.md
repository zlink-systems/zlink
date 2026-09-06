# gate r1-r2 (R1-AB stream, R2 engine/asio·protocol·tls) 요약

- 적용: `~/project/zlink-work/r1`(stream.cpp/.hpp, stream_dispatch_lifecycle.cpp 삭제, core/CMakeLists.txt) → main HEAD `7772b050cd`(pull --rebase 뒤)에 `git apply --3way` 성공, 충돌 없음. `~/project/zlink-work/r2`(asio_engine*, asio_stream_fastpath_policy.hpp, decoder*.hpp, ssl_context_helper.cpp/ssl_transport.cpp, asio_error_handler.hpp 삭제)도 충돌 없이 적용. 두 job 모두 `git diff HEAD`로 패치 생성(R2는 asio_error_handler.hpp 삭제가 index에 staged라 `git diff`(unstaged만)로는 누락됨을 확인 후 `HEAD` 기준으로 전환).
- 삭제·CMakeLists 확인: `stream_dispatch_lifecycle.cpp`, `asio_error_handler.hpp` 실제 삭제됨(파일 없음). `core/CMakeLists.txt`의 `socket-stream-sources`에서 `stream_dispatch_lifecycle.cpp` 제거 확인.
- 공개 인터페이스: `git diff --stat -- core/include core/src/libzlink.vers` 적용 전/후 모두 빈 결과 — 위반 없음.
- 빌드: `JOBS=6 scripts/build-core.sh dev` 성공. `release --lib-only` 성공.
- ctest 전체 1회: 209개 중 207 통과, 2 실패. `test_stream_socket_recv_multiclient_ready_regression`(타임아웃) 단독 3회 재실행 전부 통과 → 병렬 부하로 인한 간헐. `hotpath_gate`는 dev 트리 ctest에 포함되어 valgrind 없이 실행되어 전 셀 FAIL(예상된 방법론 불일치, step7 결과가 유효 판정).
- 변경 suite 합집합 패턴(`stream|router|dispatch|engine|asio|raw|zmp|tls|ws|decoder`) 5x: 50개 테스트 전부 5/5 통과.
- 헤더 mirror cmp: `scripts/gate/README.md`에 8헤더×4mirror 절차 없어 fallback 사용 — core/include의 zlink.h/zlink_enum.h/zlink_errno.h 3개 × bindings/{go,rust,cpp,c} 4곳 = 12조합 전부 cmp 일치.
- hotpath_gate 5셀(`core/build-gate`, valgrind, flock, load avg 4.15/1.48/1.14):

| 셀 | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3382.371 | 0.9880 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19339.941 | 0.9826 | PASS |
| pair_inproc | 2527.834 | 2504.858 | 0.9909 | PASS |
| router_router_tcp | 2972.532 | 2954.444 | 0.9939 | PASS |
| stream_tcp | 14623.471 | 14630.796 | 1.0005 | PASS |

- with_stream(`--stack zlink,asio --size all --ccu 1000 --runs 1 --reuse-build`, `ZLINK_CORE_SOURCE=local` 필요 — 기본값 release라 다운로드 404 발생해 전환, load avg 1.56/1.26/1.08):

| size | zlink kops | asio kops | zlink/asio | §7.1 Phase2S idle 기준 비율 |
|---|---:|---:|---:|---:|
| 64B | 318.19 | 394.55 | 0.806 | 0.821 |
| 1024B | 290.32 | 332.18 | 0.874 | 0.823 |
| 65536B | 35.45 | 44.48 | 0.797 | 0.787 |

절대값도 §7.1 최신 기준(289.7/267.8/32.6 zlink) 대비 비슷하거나 소폭 높음 — 회귀 없음.

- perf/c 경량 3셀(1024B tcp, runs 1, load avg 2.18/1.61/1.23 → 1.61/1.54/1.22):

| 셀 | §7.4 Phase2G idle 기준 | 측정값 | 비율 |
|---|---:|---:|---:|
| single ROUTER_ROUTER | 732.2 Kmsg/s | 829.05 Kmsg/s | 1.132 |
| multi RR_SENDSEND | 242.5 Kops/s | 305.28 Kops/s | 1.259 |
| multi RR_REQREP | 170.1 Kops/s | 201.38 Kops/s | 1.184 |

전부 기준보다 높음(회귀 없음). §7.2 표의 Phase 0 값(744.4/111.5/73.0)은 부하 오염으로 폐기된 값이라 Phase 2G idle 기준으로 비교.

- working tree: 패치 적용 상태 그대로 유지(커밋하지 않음). core/build, core/build-dev, core/build-gate 산출물 존재.
