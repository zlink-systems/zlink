# R11 인벤토리 — core/src/runtime/utils/

읽기 전용 조사. 빌드·측정 미실행. 기준: posddd.ko.md 얕은 모듈 스멜 카탈로그, 08-posd-module-structure, S-2(mutex 통합) 보고.

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|---|---|---|---|---|---|---|
| 1 | 1(dead) | `sockets/pubsub/xsub.hpp:15,81`, `xsub.cpp:78,186`, `utils/radix_tree.hpp/.cpp`(583+행) | `ZLINK_USE_RADIX_TREE`는 `builds/cmake/platform.hpp.in:85`의 `#cmakedefine` 뿐, 이를 켜는 CMake 옵션이 어디에도 없음(`grep RADIX_TREE core/CMakeLists.txt` 무결과). 즉 `#ifdef ZLINK_USE_RADIX_TREE` 분기는 이 저장소의 어떤 빌드 구성으로도 활성화 불가 — xsub는 항상 `trie_with_size_t` 경로만 사용. radix_tree.cpp/hpp는 CMakeLists.txt:1013에 의해 컴파일만 되고 어디서도 인스턴스화 참조 없음. | radix_tree 경로(및 radix_tree.cpp/hpp 전체)를 제거하거나, 반대로 CMake 옵션을 노출해 실제로 선택 가능하게 함 — 둘 중 하나를 소유자가 결정. 현재 상태는 "죽지도 살지도 않은" 코드로 유지비만 발생. | 파일 3개(xsub.hpp/cpp, radix_tree.*), CMakeLists 1줄, ~700행 | 있음→D(공개 동작 없음 확인 필요: xsub getsockopt 등에서 radix_tree 전용 옵션 노출 여부 재확인) | 없음(죽은 분기 제거는 무영향) |
| 2 | 1(dead 후보) | `utils/atomic_ptr.hpp`, `utils/atomic_counter.hpp` 전체 | S-2 보고에서 지적된 대로 `ZLINK_ATOMIC_PTR_WINDOWS`/`ZLINK_ATOMIC_COUNTER_WINDOWS`/`ZLINK_ATOMIC_*_MUTEX`/`__SUNPRO_CC` 분기는 이 빌드(POSIX+C++11)에서 비활성. 다만 이들은 Windows·컴파일러-미지원-C++11 플랫폼용 실코드이므로 "이 빌드에서 비활성"과 "모든 지원 플랫폼에서 죽음"은 다름. | 제거 제안 없음 — 저장소의 공식 지원 플랫폼 목록을 찾지 못해(문서 미확인) 각 분기의 생사 판정 보류. | — | — | — |
| 3 | 5(확인 필요) | `utils/ip_fdpair.cpp:28-35` (`ZLINK_HAVE_OPENVMS`, `ZLINK_HAVE_VXWORKS` 분기) | libzmq 유산으로 보이는 OpenVMS/VxWorks 전용 코드. zlink가 이 플랫폼들을 실제로 지원하는지 미확인. | 확인 필요 — 지원 플랫폼 문서(또는 CI 매트릭스)에 OpenVMS/VxWorks가 없으면 제거 후보로 재상정. | 파일 1개, ~20행 | 있음(플랫폼 지원 범위 축소면 D) | 없음 |
| 4 | (검증 결과: 정상) | `utils/mutex.hpp` | S-2가 fast_mutex를 mutex_t/recursive_mutex_t로 통합한 뒤 `fast_mutex` 잔여 참조를 저장소 전체에서 grep했으나 0건 — 후속 잔재 없음. 주석도 정상 상태를 명시적으로 설명. | 없음(양호) | — | 없음 | 없음 |
| 5 | (검증 결과: 정상) | `core/ypipe_conflate.hpp`, `core/pipe.cpp:98,115,137-147,3221-3222,3782-3826` | dbuffer/ypipe_conflate는 radio/dish가 아니라 `ZMQ_CONFLATE` 소켓 옵션 경로에서 여전히 실사용(conflate_ 플래그로 pipe.cpp가 선택). 죽은 코드 아님. | 없음 | — | 없음 | 없음 |
| 6 | (검증 결과: 정상) | `mtrie.hpp/.cpp` vs `trie.hpp/.cpp` vs `radix_tree.hpp/.cpp` | 셋 다 실제로는 서로 다른 소비자를 가짐: xpub → generic_mtrie 기반 mtrie_t, xsub → trie_with_size_t(#1로 인해 radix_tree는 도달 불가). generic_mtrie는 mtrie.hpp의 단일 인스턴스화(`generic_mtrie_t<pipe_t>`) 외 다른 소비자 없음(1:1 wrapper이므로 얕은 모듈 소지 있으나 확장점 의도로 보임 — 확인 필요). | mtrie.hpp가 하는 일은 `extern template` 선언 + typedef 뿐인 얇은 래퍼. 다른 인스턴스화가 없다면 generic_mtrie/mtrie 분리의 실익 재검토 후보(확인 필요, 이번 R11에서는 결론 보류). | 파일 2개, ~30행 | 없음 | 없음 |

## 요약
- 카테고리별: 1(dead code) 1건 확정(#1) + 1건 확인필요 승격 대상(#3), 3(얕은 모듈) 확인필요 1건(#6 mtrie/generic_mtrie 래퍼), 나머지 리드(S-2 fast_mutex, dbuffer/conflate)는 검증 결과 문제 없음으로 종결.
- 항목 수가 적은 이유: R11의 40개 파일 중 절대다수(clock/err/random/ip/polling_util/routing_id/blob/compat/env/heap_owner/array 등)는 실사용처가 다수이고 뚜렷한 중복·죽은 코드 신호가 없어 표에서 제외(허위 dead-code 판정 방지 우선, 브리프 지시대로).

## 적용 job 묶음 제안
- **묶음 A (파일: xsub.hpp/.cpp, radix_tree.hpp/.cpp, CMakeLists.txt 1013행)** — 항목 #1. radix_tree 죽은 분기 처리(제거 또는 옵션 노출) 결정 후 적용. 계약 확인(D) 선행 필요 — 1.5h 내 가능.
- **묶음 B (파일: ip_fdpair.cpp)** — 항목 #3. OpenVMS/VxWorks 지원 여부 확인 후, 미지원이면 분기 제거. 확인 단계가 선행되므로 별도 job으로 분리(플랫폼 지원 문서/CI 매트릭스 확인 → 제거는 후속 job).
- (묶음 A/B는 파일이 겹치지 않음. #6은 결론 보류 상태라 이번 라운드 적용 대상에서 제외 — 필요 시 별도 조사 job으로.)

보고 경로: `doc/plan/c016-worklog/core-rf-R11-inventory.md`
