# C++ contract socket test 수정 요약

## 결과

`test_concurrent_pair_multipart_exposes_core_rejection_and_returns_lvalues`가 receiver drain 전에 기본 1 MiB HWM을 채워 멈추던 회귀를 test-only 변경으로 제거했다. 수정한 repository source는 `bindings/cpp/tests/contract/test_cpp_contract_socket.cpp` 한 파일이며 diff는 +74/-1이다.

## 변경 diff 요약

- bind/connect 전에 sender `SNDHWM`과 receiver `RCVHWM`을 C++ public option facade로 각각 64 MiB로 설정했다. 8 thread가 만드는 최대 약 3,429,360 B보다 충분히 커서 이 테스트가 검증 대상이 아닌 receiver credit에 막히지 않는다.
- 원래 thread가 public C part-send의 `MORE`를 성공시켜 multipart sequence를 연 상태에서 다른 thread가 C++ 3-part builder를 제출하는 결정적 보조 케이스를 추가했다.
- 보조 케이스는 contender가 `invalid_argument/EINVAL`로 거절되는지, 거절된 세 caller-owned lvalue가 원래 payload를 그대로 소유하는지 확인한다.
- 원래 thread가 `FINAL`을 보낸 뒤 held 2-part record를 온전히 받는지 확인한다. 이로써 contender record가 열린 sequence에 interleave되지 않았고 기존 sequence가 손상되지 않았음도 함께 고정한다.
- 기존 8 x 2,000 동시 multipart stress는 accepted/rejected 총합, 발생한 모든 rejection의 `invalid_argument/EINVAL` 분류와 lvalue 복원, accepted 3-part record 무결성 검사를 유지한다. scheduler timing에만 기대던 `rejected_count > 0` assertion은 제거했다.

결정적 precondition을 만들 때만 기존 contract test에서 쓰는 test-only native-handle access를 사용했다. 실제 거절·예외·lvalue 복원 검사는 C++ public builder surface에서 수행한다. 판정 근거는 같은 socket의 동시 send 허용, multipart는 한 thread의 단일 sequence이며 다른 record의 part가 사이에 섞이지 않아야 한다는 공개 계약이다.

## 반복 검증

모든 build/test 실행 전에 `ulimit -v 16777216`을 적용했다. `scripts/local-package/**`와 perf는 실행하지 않았다.

### `test_cpp_contract_socket` 단독

- focused build: PASS, 2.22초.
- 수정 직후 precheck: PASS, 0.18초.
- 요청된 연속 20회: PASS 20/20.
- 각 실행 시간(초): 0.19, 0.19, 0.19, 0.18, 0.19, 0.19, 0.18, 0.19, 0.19, 0.19, 0.19, 0.18, 0.18, 0.19, 0.19, 0.18, 0.18, 0.18, 0.19, 0.16.
- 범위: 0.16-0.19초. 기존 pristine 0.17초 수준과 같으며 수십 초짜리 HWM 대기는 관찰되지 않았다.

### `bash bindings/cpp/tests/run_tests.sh`

연속 5회 모두 contract 15/15 PASS였다. runner가 함께 실행한 sample smoke도 매회 7/7 PASS였다.

| 회차 | contract | CTest contract time | sample smoke | 전체 runner wall time |
|---:|---:|---:|---:|---:|
| 1 | 15/15 PASS | 1.45초 | 7/7 PASS | 9.51초 |
| 2 | 15/15 PASS | 1.46초 | 7/7 PASS | 9.88초 |
| 3 | 15/15 PASS | 1.45초 | 7/7 PASS | 9.12초 |
| 4 | 15/15 PASS | 1.44초 | 7/7 PASS | 6.89초 |
| 5 | 15/15 PASS | 1.44초 | 7/7 PASS | 9.81초 |

추가 정적 확인: `git diff --check -- bindings/cpp/tests/contract/test_cpp_contract_socket.cpp` PASS.

## 범위

- 변경: `bindings/cpp/tests/contract/test_cpp_contract_socket.cpp`
- 기록: `/home/hep7/project/zlink-work/c016/cpp-contract-socket-test-progress.md`, `/home/hep7/project/zlink-work/c016/cpp-contract-socket-test-summary.md`
- 변경하지 않음: `core/**`, `doc/**`, `framework/**`, 다른 binding, `bindings/cpp/src/**`, `bindings/cpp/include/**`, 같은 디렉터리 공용 헬퍼.
- git write, commit, push 없음. 기존 `core/**` staged/unstaged 변경은 손대지 않았다.

## QUESTIONS

없음.
