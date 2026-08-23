# C++ `PUBSUB`/`tcp` Sol 리뷰 후속과 최종 판정 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> §5(고정 원칙), §7.2(반복 횟수 — `최종·경계 판정`), §7.3(paired C 규칙), §7.5, §8(판정), §9.1.1
>
> 선행 기록:
> `log/2026-08-23-cpp-pubsub-tcp-official.md`(첫 공식 측정 89.40%),
> `log/2026-08-23-cpp-pubsub-improvement-pass.md`(자체 개선 pass, 재측정 86.15%),
> `log/2026-08-23-sol-review-pubsub-tcp.md`(Sol read-only 리뷰)
>
> 이 문서는 Sol 리뷰가 지시한 순서 — (1) P2 contract-order 수정, (2) LTO/IPO A/B,
> (3) §7.2 5-run 최종·경계 판정 — 를 그대로 수행한 기록이다. commit·push는 하지 않았다.

## 1. 환경 manifest

| 항목 | 값 |
|------|-----|
| host | `ulalax-gram` |
| OS/kernel | `Linux 6.6.87.2-microsoft-standard-WSL2` (WSL2) |
| CPU | `x86_64`, 16 logical cores (`12th Gen Intel(R) Core(TM) i7-1260P`) |
| memory | 11Gi total |
| 컴파일러 / CMake | `gcc 13.3.0` (Ubuntu 13.3.0-6ubuntu2~24.04.1) / `cmake 3.28.3` |
| 작업 브랜치 / commit | `codex/bindings-0.12.0-performance` / `f99703c2190b0f6c670be49f67315d904886c742`(HEAD, 미커밋 변경 포함) |
| Core runtime | release `0.12.0`, prefix `/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64` |
| Core release tag / provenance revision | `core/v0.12.0` / `f99703c2190b0f6c670be49f67315d904886c742`, `dirty: false` |
| C reference 바이너리 | `bindings/c/build-release-0.12.0/perf/perf_pubsub` |
| session tag (A/B) | `bindings-0.12.0-ltoab-off-20260823`, `bindings-0.12.0-ltoab-on-20260823` |
| session tag (최종) | `bindings-0.12.0-official-pubsub-final5-20260823` |
| 시각 | 2026-08-23 15:12 ~ 15:33 KST |

---

# STEP 1 — P2 contract-order 수정

## 2. 무엇이 문제였고 어떻게 고쳤나

Sol 리뷰 §4의 지적은 다음과 같다. P2가 도입한 `make_publish_state()`는
`pub_socket_t::publish()` / `xpub_socket_t::publish()`에서 **인자로** 호출되므로,
컴파일러가 `callback_state()`를 topic validation보다 **먼저** 평가할 수 있다.
`callback_state()`는 `_callbacks`가 없으면 lazy allocation을 수행하므로
(`socket.cpp:611`), moved-from socket에 embedded-null topic을 넘기는 경우
validation 예외 대신 callback state 생성이나 `bad_alloc`이 먼저 관측될 수 있다.
P2 이전 코드는 validation이 항상 첫 단계였다.

helper를 **callback state를 반환하는 호출 가능 객체**를 받도록 바꿔, P2 이전의
관측 가능한 단계 순서를 그대로 복원했다. 중복 제거 구조(PUB/XPUB이 조립 절차를
한 번만 기술)는 유지된다.

```cpp
template <class callback_state_fn_t>
std::unique_ptr<detail::operation_state_t>
make_publish_state (socket_t &socket_,
                    const std::string &topic_id_,
                    callback_state_fn_t &&callback_state_)
{
    detail::validate_no_embedded_null (topic_id_, "topic");   // 1. 항상 먼저
    auto state_ptr = detail::acquire_state ();                 // 2.
    state_ptr->kind = detail::operation_kind_t::raw_publish;
    state_ptr->raw.socket = detail::native_handle (socket_);   // 3.
    detail::bind_callback_state (state_ptr->raw, callback_state_ ());  // 4. 여기서 최초 평가
    state_ptr->raw.topic = topic_id_;                          // 5.
    return state_ptr;
}
```

두 `publish()`는 `*this`와 topic, 그리고 `[this] () -> detail::socket_callback_state_t &
{ return callback_state (); }` 람다를 넘긴다. 공개 signature·type·ownership·예외 타입·
`internal_errno`는 모두 불변이고, 정상 소켓의 hot path 호출 시퀀스도 동일하다
(같은 TU의 template helper라 `-O3`에서 인라인된다).

## 3. STEP 1 계약 검증

| 검증 | 결과 |
|------|------|
| contract 테스트 (`bindings/cpp/build`, `test_cpp_contract_*` 14개) | **12 통과 / 2 실패** — 기존과 동일 |
| 실패 2건 | `test_cpp_contract_request_reply`, `test_cpp_contract_socket`(둘 다 pre-existing, `log/2026-08-23-c6-fix-and-64k-diagnosis.md` §4.2와 같은 2건) |
| sample smoke (`ctest -L sample-smoke`, 7개) | **6 통과 / 1 실패** — `sample_smoke_sample_cpp_dealer_router_recv_sample`(pre-existing) |
| 빌드 경고 | 0 |

기대했던 `12/14`, `6/7` 패턴 그대로다.

---

# STEP 2 — LTO/IPO A/B

## 4. 배선 (build option 변경만)

Sol 리뷰 §1의 지적대로 `ENABLE_LTO`는 C++ binding에서 **어떤 IPO 속성에도 연결돼
있지 않았다**. runner가 넘기던 `-DENABLE_LTO=OFF`는 아무 효과가 없었고, 그래서
`ON`으로 바꿔도 아무 일도 일어나지 않았다. 두 곳을 배선했다.

| 파일 | 변경 |
|------|------|
| `bindings/cpp/CMakeLists.txt` | `option(ENABLE_LTO ... OFF)` 정의(이미 `-D`로 주어졌으면 그 값 사용) → `ENABLE_LTO`가 켜졌을 때만 `include(CheckIPOSupported)` + `check_ipo_supported()`로 확인하고, 성공 시 `ZLINK_CPP_IPO_ENABLED=ON`. 이 경우에만 `zlink_cpp` 라이브러리 target에 `INTERPROCEDURAL_OPTIMIZATION TRUE` 설정 |
| `bindings/cpp/perf/CMakeLists.txt` | `configure_perf_target()`에서 `ZLINK_CPP_IPO_ENABLED`일 때만 같은 속성을 perf 실행파일에 설정. static 라이브러리만 LTO로 빌드하면 최종 링크에서 TU 경계가 그대로 남으므로 최종 consumer에도 필요하다 |

**공개 API·소스 계약 변경은 없다.** 기본값 `OFF`에서는 어떤 target에도 속성을
설정하지 않으므로 오늘까지의 산출물과 동일하다(확인: `ENABLE_LTO=OFF`로 재구성한 뒤
`CMakeFiles/zlink_cpp.dir/flags.make`에 `-flto` 0건). `ON`에서는 라이브러리와
`cpp_perf_pubsub` 양쪽의 컴파일·링크 커맨드에 `-flto=auto`가 들어간다(확인:
`perf/CMakeFiles/cpp_perf_pubsub.dir/link.txt`, `flags.make` 모두 `-flto=auto`).

## 5. A/B 실행 조건

공식 조건 그대로다. 각 variant마다 **C reference를 같은 세션에서 paired로 재측정**했다.

```bash
# variant A (ENABLE_LTO=OFF) — runner 기본 경로가 -DENABLE_LTO=OFF로 재구성
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 --pattern PUBSUB --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-ltoab-off-20260823
PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --core-version 0.12.0 --pattern PUBSUB --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-ltoab-off-20260823

# variant B (ENABLE_LTO=ON) — 공식 build dir를 LTO로 재구성한 뒤 --reuse-build
cmake -S bindings/cpp -B bindings/cpp/build -DENABLE_LTO=ON
cmake --build bindings/cpp/build -j8
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh ... \
  --results-tag bindings-0.12.0-ltoab-on-20260823
PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh --reuse-build ... \
  --results-tag bindings-0.12.0-ltoab-on-20260823
```

`--reuse-build`를 쓴 이유는 runner의 기본(incremental) 경로가 항상
`-DENABLE_LTO=OFF`로 재구성하기 때문이다. 네 report 모두 `status: complete`, 30/30 result lines.

## 6. A/B 결과

| Size | variant A (`ENABLE_LTO=OFF`) C++/C | variant B (`ENABLE_LTO=ON`) C++/C | B − A |
|-----:|-----------------------------------:|----------------------------------:|------:|
| 64 | 86.24% | **88.97%** | +2.73%p |
| 256 | 93.45% | 93.46% | +0.01%p |
| 1024 | 91.26% | **93.22%** | +1.96%p |
| 65536 | **87.54%** | 85.19% | −2.35%p |
| 131072 | **90.09%** | 83.13% | **−6.96%p** |
| 262144 | 88.40% | **90.99%** | +2.59%p |
| **aggregate throughput mean** | **89.50%** | **89.16%** | **−0.34%p** |
| **aggregate latency mean** | **1.060배** | 1.079배 | +0.019배(악화) |

원시 median(msg/s):

| Size | A: C | A: C++ | B: C | B: C++ |
|-----:|-----:|-------:|-----:|-------:|
| 64 | 1,546,628.6 | 1,333,762.2 | 1,470,595.6 | 1,308,374.2 |
| 256 | 1,191,213.8 | 1,113,164.8 | 1,172,928.0 | 1,096,181.6 |
| 1024 | 1,078,454.6 | 984,174.2 | 1,074,221.0 | 1,001,366.8 |
| 65536 | 45,732.6 | 40,034.8 | 49,014.6 | 41,753.8 |
| 131072 | 23,983.0 | 21,607.2 | 25,129.6 | 20,889.6 |
| 262144 | 14,960.6 | 13,225.4 | 14,776.4 | 13,445.6 |

### 6.1 STEP 2b — LTO 빌드 miscompile 확인

LTO-ON 빌드에 대해 같은 계약 검증을 다시 실행했다.

| 검증 | LTO-OFF | LTO-ON |
|------|---------|--------|
| contract 14개 | 12 통과 / 2 실패 | **12 통과 / 2 실패 (동일)** |
| 실패 2건 | `request_reply`, `socket` | **같은 2건** |
| 14개 테스트의 표준출력/표준오류 | — | **모두 `diff` 완전 동일** |
| sample smoke 7개 | 6 통과 / 1 실패 | **6 통과 / 1 실패 (동일)** |

LTO로 인한 동작 차이는 관측되지 않았다. 다만 LTO 링크 단계에서 multi suite
헤더(`perf/multi/common/perf_multi_reqrep.hpp:355`)의 기존 `unused parameter` 경고가
표면화된다(single suite와 무관, 기존 코드의 경고).

### 6.2 판정 — **LTO no-go**

Sol의 채택 조건은 "**회귀 없이** aggregate를 개선할 때만 채택"이다. 결과는
aggregate throughput이 **개선되지 않고**(−0.34%p) latency도 악화하며,
65536B·131072B에서 뚜렷한 회귀가 있다. 64B·1024B·262144B의 개선은 있으나
합계가 음수이므로 조건을 만족하지 못한다.

**따라서 LTO는 no-go이며, 공식 측정 구성은 `ENABLE_LTO=OFF`(기존과 동일)를 유지한다.**
perf runner 스크립트(`run_binding_single.sh`, `run_binding_multi.sh`)는 **변경하지 않았다**
— 지금도 `-DENABLE_LTO=OFF`를 넘기며, 이번 배선으로 그 지시가 비로소 실제 효력을 갖게 됐다.

CMake 배선 자체는 남긴다. 옵션이 이름값을 하지 못하고 조용히 무시되던 상태를
고치는 것이고, 기본값 `OFF`에서 산출물이 동일하기 때문이다.

---

# STEP 3 — §7.2 최종·경계 판정 (5-run)

## 7. 실행 조건

최종 선택된 빌드는 **`ENABLE_LTO=OFF`**다(§6.2). 최종 판정 직전에 build dir를
`-DENABLE_LTO=OFF`로 되돌려 재빌드했고(`-flto` 0건 확인), C → C++ 순서로 **한 세션**에서
실행했다. 계획서 §7.2의 `최종·경계 판정` 기본 조건: default duration(5초), `--runs 5`,
CPU pin 없음.

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 --pattern PUBSUB --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 5 \
  --results-tag bindings-0.12.0-official-pubsub-final5-20260823

PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --core-version 0.12.0 --pattern PUBSUB --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 5 \
  --results-tag bindings-0.12.0-official-pubsub-final5-20260823
```

| 대상 | 결과 파일 | status | result lines |
|------|-----------|--------|--------------|
| C | `perf_c_single_linux_20260823_152615_bindings-0.12.0-official-pubsub-final5-20260823.txt` | **complete** | 30/30 |
| C++ | `perf_cpp_single_linux_20260823_152923_bindings-0.12.0-official-pubsub-final5-20260823.txt` | **complete** | 30/30 |

- Effective Options: `lang`을 제외한 **모든 항목이 `diff`로 완전히 일치**(runs 5, duration 5s, io_threads 1, auto-hwm 등).
- auto-HWM: C++ report의 `## Auto-HWM Detail`이 pass2 report와 **바이트 단위로 동일**.
- client 수: publisher 1 / subscriber 1, memory guard cap 없음.
- Core runtime: C report의 `META,core_source,release`, `META,core_release_tag,core/v0.12.0`,
  `META,commit,f99703c219`, `META,build,Release`. (C++ single report는 이전 회차들과 마찬가지로
  `META` 블록을 출력하지 않는다. C++ 쪽 core는 runner가 준비한 release runtime 디렉터리로
  고정된다.)

## 8. 원시 반복값 (5회)

C (Kmsg/s / latency mean ms):

| Size | run1 | run2 | run3 | run4 | run5 | median |
|-----:|-----:|-----:|-----:|-----:|-----:|-------:|
| 64 | 1444.48 / 26.899 | 1473.56 / 25.237 | 1478.65 / 33.206 | 1539.21 / 36.859 | 1478.14 / 29.409 | **1478.14 / 29.409** |
| 256 | 1155.58 / 28.800 | 1142.02 / 28.185 | 1150.43 / 29.090 | 1147.40 / 28.574 | 1184.03 / 29.338 | **1150.43 / 28.800** |
| 1024 | 1110.15 / 8.609 | 1070.37 / 8.830 | 1082.36 / 8.802 | 1098.72 / 8.637 | 1068.90 / 8.915 | **1082.36 / 8.802** |
| 65536 | 50.59 / 3.039 | 51.73 / 2.980 | 48.55 / 3.142 | 48.83 / 3.129 | 51.08 / 3.006 | **50.59 / 3.039** |
| 131072 | 23.28 / 3.307 | 23.54 / 3.272 | 24.90 / 3.106 | 25.80 / 3.009 | 24.11 / 3.198 | **24.11 / 3.198** |
| 262144 | 14.87 / 2.646 | 14.75 / 2.669 | 14.93 / 2.640 | 14.84 / 2.644 | 15.27 / 2.578 | **14.87 / 2.644** |

C++ (Kmsg/s / latency mean ms):

| Size | run1 | run2 | run3 | run4 | run5 | median |
|-----:|-----:|-----:|-----:|-----:|-----:|-------:|
| 64 | 1403.41 / 26.160 | 1350.62 / 25.674 | 1313.44 / 22.375 | 1287.80 / 21.319 | 1353.09 / 31.507 | **1350.62 / 25.674** |
| 256 | 1081.36 / 28.047 | 1116.91 / 25.567 | 1069.80 / 27.582 | 1083.26 / 23.252 | 1076.62 / 29.362 | **1081.36 / 27.582** |
| 1024 | 977.97 / 9.743 | 1006.18 / 9.418 | 1011.67 / 9.348 | 998.46 / 9.465 | 968.23 / 9.839 | **998.46 / 9.465** |
| 65536 | 39.20 / 3.907 | 43.22 / 3.571 | 36.90 / 4.190 | 37.87 / 4.078 | 38.56 / 4.011 | **38.56 / 4.011** |
| 131072 | 20.43 / 3.862 | 20.28 / 3.873 | 20.54 / 3.810 | 19.73 / 3.988 | 20.35 / 3.850 | **20.35 / 3.862** |
| 262144 | 14.52 / 2.715 | 12.98 / 3.029 | 13.75 / 2.856 | 13.14 / 3.009 | 12.46 / 3.144 | **13.14 / 3.009** |

## 9. 비율 (판정 입력)

| Size | C median (msg/s) | C++ median (msg/s) | **ratio** | latency ratio | 개별 기준 85% |
|-----:|-----------------:|-------------------:|----------:|--------------:|---------------|
| 64 | 1,478,144.4 | 1,350,619.6 | **91.37%** | 0.873배 | 통과 |
| 256 | 1,150,433.4 | 1,081,355.0 | **94.00%** | 0.958배 | 통과 |
| 1024 | 1,082,355.4 | 998,457.2 | **92.25%** | 1.075배 | 통과 |
| 65536 | 50,591.0 | 38,555.6 | **76.21%** | 1.320배 | 미달 — **환경 지배**(§10) |
| 131072 | 24,112.4 | 20,350.8 | **84.40%** | 1.208배 | 미달(경계) |
| 262144 | 14,874.6 | 13,140.0 | **88.34%** | 1.138배 | 통과 |
| **aggregate mean** | | | **87.76%** | **1.095배** | |

**기록용(판정 입력 아님)**: 65536B 제외 5셀 aggregate throughput mean = **90.07%**.
`MALLOC_*` 조건의 94.31%도 공식 결과가 아니다. 계획서 §8은 측정된 모든 size의
산술평균으로 판정하므로 두 수치 모두 판정에 사용하지 않는다.

## 10. 65536B 셀 기록 방식 (Sol §2)

`76.21%`를 **측정값 그대로** 남기고 **aggregate에도 포함**한다. `통과`·`해당 없음`·
`환경 제외`로 바꾸거나 5-cell 평균으로 대체하지 않는다. 동시에 원인은 별도로 표시한다.

- 셀 표기: `보류(76.21%; 환경 지배)`
- 근거(이미 `log/2026-08-23-cpp-pubsub-improvement-pass.md` §2~§4에 기록): C++ 프로세스의
  minor page fault가 C의 약 **540배**(1.13~1.36M vs 약 2.1k), `MALLOC_TRIM_THRESHOLD_`만
  키우면 fault가 붕괴하며 비율이 **94.31%**로 상승, 반대로 C를 같은 fault 체제에 넣으면
  **C가 C++보다 느려진다(C++/C = 122.20%)**.
- 즉 "**환경 원인이지 binding 결함의 증거는 아니다**"와 "**aggregate throughput은 미달이다**"를
  동시에 기록한다.

Sol §3의 지시대로 65536B만 따로 재실행하거나 유리한 라운드를 고르지 않았다. 이 세션의
5-run 결과가 그대로 최종값이다.

## 11. 최종 판정

| 항목 | 값 | 기준 | 결과 |
|------|-----|------|------|
| throughput aggregate mean | **87.76%** | 목표 95% / 완화 90% | **미달** |
| 개별 size 최소 기준 | 65536B 76.21%(환경 지배), 131072B 84.40% | 85% | 2개 셀 미달 |
| latency aggregate mean | **1.095배** | 상한 2.0배 | **통과** |
| Effective Options / auto-HWM / client 수 | 일치 | — | 통과 |
| paired report status | complete / complete | — | 통과 |

**최종 상태: `보류(87.76%)`.**

계획서 §8의 정의상 두 개선 pass(자체 pass + Sol 리뷰 pass)가 모두 끝나고 contract를
보존하는 잔여 후보가 없을 때의 상태는 `미달`이 아니라 `보류`다. 이번 pass로 두 조건이
모두 충족됐다.

- 자체 pass: P1 채택(측정 의미 정렬), P2 채택(중복 제거) + 이번 S1으로 contract-order 복원,
  P3~P7 no-go.
- Sol 리뷰 pass: builder state stack/SBO 전환·3-call 병합·`weak_ptr::expired()` 호이스팅
  모두 no-go 재확인, 유일한 남은 후보였던 **LTO/IPO는 A/B 결과 no-go**.

`보류`는 통과를 의미하지 않는다. 목표 95%와 완화 90%에 모두 미달한 상태로 기록을 확정한다.

### 11.1 세 차례 공식 측정 이력

| 회차 | 조건 | aggregate throughput | aggregate latency |
|------|------|---------------------:|------------------:|
| 첫 공식 측정 | runs=3 (후보 판정) | 89.40% | 1.062배 |
| 자체 pass 후 재측정 | runs=3 (후보 판정) | 86.15% | 1.120배 |
| LTO A/B variant A | runs=3 (후보 판정) | 89.50% | 1.060배 |
| LTO A/B variant B (LTO ON) | runs=3 (후보 판정) | 89.16% | 1.079배 |
| **§7.2 최종·경계 판정** | **runs=5, default duration, pin 없음** | **87.76%** | **1.095배** |

회차 간 3%p대의 변동은 §3.2(개선 pass 로그)에서 정량화한 glibc allocator 체제 변동이
대형 크기 셀에 주는 영향이다. 계획서 §7.5에 따라 변동을 이유로 반복 측정하지 않고,
§7.2가 규정한 최종 조건의 1회 측정값으로 확정한다.

## 12. 코드·commit 상태

### 12.1 저장소에 남긴 변경

| 파일 | 종류 |
|------|------|
| `bindings/cpp/src/Runtime/Sockets/pubsub.cpp` | **shipping code — S1**(P2 contract-order 복원, 공개 API 불변, 성능 중립) |
| `bindings/cpp/CMakeLists.txt` | **build option — S2**(`ENABLE_LTO` → `INTERPROCEDURAL_OPTIMIZATION` 배선, 기본 `OFF`) |
| `bindings/cpp/perf/CMakeLists.txt` | **build option — S2**(perf 실행파일에 동일 속성, 기본 `OFF`) |
| `doc/perf/perf/bindings-0.12.0/log/2026-08-23-cpp-pubsub-final-judgment.md` | 이 문서 |
| `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md` §9.1.1 | `tcp`/`PUBSUB` 행을 5-run 최종값·`보류`로 갱신 |
| `doc/perf/perf/bindings-0.12.0/progress.ko.md` | §3·§4·§5(§5.2 신설) 갱신 |
| `bindings/c/perf/results/single/report/perf_c_single_linux_20260823_151612_*.txt` | LTO A/B variant A — C |
| `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260823_151807_*.txt` | LTO A/B variant A — C++ |
| `bindings/c/perf/results/single/report/perf_c_single_linux_20260823_152115_*.txt` | LTO A/B variant B — C |
| `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260823_152311_*.txt` | LTO A/B variant B — C++ |
| `bindings/c/perf/results/single/report/perf_c_single_linux_20260823_152615_*.txt` | **최종 5-run — C** |
| `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260823_152923_*.txt` | **최종 5-run — C++** |

**perf runner 스크립트는 변경하지 않았다** — LTO가 no-go이므로 runner의
`-DENABLE_LTO=OFF` 기본값이 그대로 공식 측정 구성이다.

**commit·push 하지 않았다.**

### 12.2 최종 build 상태

작업 종료 시점의 `bindings/cpp/build`는 `ENABLE_LTO=OFF`로 재구성·재빌드된 상태이며,
이것이 §7~§11 최종 측정에 사용된 빌드다.
