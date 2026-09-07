# MP 경량 게이트 요약

## 적용·범위

- 재실행 지시를 따라 patch apply는 생략했다. MP 변경은 이미 main index에 staged되어 있었고 충돌은 없다.
- `core/doc/spec/**`의 감독관 미커밋 수정은 제외·무변경으로 보존했다. staged MP 범위는 tracked 30개와 추가 테스트 1개(`test_writable_resubmit_from_other_thread_while_sequence_open.cpp`)다.
- 대상: `part_helper_*` 3개, socket API/request-reply 9개, common socket 6개, `core/tests/CMakeLists.txt`, integration 6개, unittest 6개. `git diff --cached --check` PASS.

## 빌드·CTest

| 명령 | 결과 |
|---|---|
| `JOBS=4 scripts/build-core.sh dev` | PASS |
| 전체 `ctest --test-dir core/build-dev -j2 --output-on-failure` | 기능 **209/209 PASS**, 248.73초; dev-tree `hotpath_gate` 1건만 FAIL |
| 변경 suite 정규식 1회 | **97/97 PASS**, 140.63초 |
| 변경 suite 정규식 2회 | **97/97 PASS**, 143.79초 |
| 변경 suite 정규식 3회 | **97/97 PASS**, 141.08초 |
| `JOBS=4 scripts/build-core.sh release --lib-only` | PASS |

전체 CTest의 유일한 실패는 측정 reference 범위 밖인 `hotpath_gate`이며 지시의 "hotpath_gate 제외 정상"에 해당한다. 따라서 기능 실패 재실행 대상은 없었다. 신규 writable-resubmit 두 case는 전체 CTest에서 모두 PASS했다.

## 공개 인터페이스

- `git diff --cached --stat -- core/include core/src/libzlink.vers`: 출력 없음.
- `scripts/gate/README.md`에 mirror 절차가 없어 fallback을 사용했다. Core `zlink.h`·`zlink_enum.h`·`zlink_errno.h`와 C/C++/Go/Rust mirror를 `cmp`로 비교해 **12/12 PASS**했다.

## hotpath 5셀

`cmake --build core/build-gate --target hotpath_bench -j4` 후 `PERF_LOCK` 아래 1회 실행했다. 시작 load는 `3.72/1.68/1.00`이다.

| cell | reference Ir/msg | 측정 Ir/msg | ratio | 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3230.922 | 3287.917 | 1.0176 | PASS |
| dealer_router_reqrep_inproc | 18663.506 | 16455.383 | 0.8817 | FAIL* |
| pair_inproc | 2348.457 | 2367.778 | 1.0082 | PASS |
| router_router_tcp | 2972.532 | 2966.839 | 0.9981 | PASS |
| stream_tcp | 14623.471 | 13969.806 | 0.9553 | PASS |

\* MP-9가 귀속한 request/reply instruction 감소(−10% 이상)로, 지시대로 FAIL 표기만 하고 중단하지 않았다.

## with_stream

`PERF_LOCK`, `ZLINK_CORE_SOURCE=local`, `--stack zlink,asio,zmq --size all --ccu 1000 --runs 1 --reuse-build` 1회다. load 조건은 대기 뒤 `0.78/1.23/0.91`에서 충족했고, 측정 시작은 `0.51/1.13/0.89`였다. 결과 디렉터리는 `bindings/c/bench/with_stream/results/20260908_032921/`이며 모든 셀 mismatch는 0이다.

| size | zlink kops | asio kops | zmq kops | §7.1 idle zlink | idle 대비 |
|---:|---:|---:|---:|---:|---:|
| 64 B | 301.775 | 386.784 | 342.827 | 298.5 | +1.10% |
| 1024 B | 275.228 | 340.188 | 308.025 | 277.5 | −0.82% |
| 65536 B | 33.786 | 40.494 | 28.878 | 33.9 | −0.34% |

최종 관측 load는 `1.55/1.52/1.07`이다. perf/c는 지시대로 생략했다. patch는 staged 상태로 유지했고 commit·stash·spec 수정은 하지 않았다.
