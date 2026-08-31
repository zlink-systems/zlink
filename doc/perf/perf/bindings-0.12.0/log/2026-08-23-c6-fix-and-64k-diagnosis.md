# C6 `_has_payload` invariant 수정과 64KiB 차분 진단 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> 선행 리뷰: `log/2026-08-23-sol-review-pair-tcp.md` (C6 = GO prerequisite, C8 = 진단 후보)
> 선행 profile 방법론: `log/2026-08-23-cpp-pair-tcp-profile.md` §3 (경로 치환 차분)
>
> 이 문서는 두 가지를 기록한다.
>
> 1. **TASK A** — C6 correctness 결함 수정과 검증 (shipping code 변경 1개 파일)
> 2. **TASK B** — C8 64KiB 차분 진단 (shipping code 변경 **없음**, throwaway 계측)
>
> commit·push는 하지 않았다.

## 1. 환경 manifest

| 항목 | 값 |
|------|-----|
| host | `ulalax-gram` |
| OS/kernel | `Linux 6.6.87.2-microsoft-standard-WSL2` (WSL2) |
| CPU | `12th Gen Intel(R) Core(TM) i7-1260P`, 16 logical cores |
| CPU governor | 읽을 수 없음 (WSL2에 cpufreq 미노출), CPU pin 없음 |
| 작업 브랜치 | `codex/bindings-0.12.0-performance` |
| working tree | C1/C3/C4/C5 채택 후보 + 이번 C6 수정이 반영된 미커밋 상태 |
| Core runtime (공식 재측정·ablation) | release `--core-version 0.12.0`, `/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64/lib/libzlink.so.0.12.0` |
| Compiler | `g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| profiler 가용성 | `perf`·`valgrind` **미설치**, `/proc/sys/kernel/perf_event_paranoid = 2` — 샘플링 프로파일 불가 |
| session tag | `c6-followup-20260823` |

`perf`/`valgrind`를 쓸 수 없으므로 Sol이 지정한 C8의 `perf record -g` / `perf stat`
방법 대신 `log/2026-08-23-cpp-pair-tcp-profile.md` §3의 **경로 치환 차분(ablation)**
을 그대로 재사용했다. 따라서 아래 §4의 "비중"은 함수별 CPU 샘플 백분율이 아니라
**해당 계층을 C API 직접 호출로 치환했을 때 회복되는 처리량의 전체 gap 대비 비율**
이다.

---

# TASK A — C6: `_has_payload` native move invariant 복구

## 2. 결함

### 2.1 메커니즘

[`message_access_t::move_to_native()`](../../../../../bindings/cpp/src/Runtime/Native/message_access.hpp#L67)
는 native frame을 밖으로 내보내면서 source의 `_valid`와 `_has_payload`를 모두
`false`로 만든다. 이것은 올바르다.

반대 방향이 문제였다.
[`native_message_parts.hpp`](../../../../../bindings/cpp/src/Runtime/Native/native_message_parts.hpp)
의 복구·materialization helper들은 `message_t::init()`(역시 `_has_payload=false`로
만든다) 뒤에 `zlink_msg_move()`로 native payload를 집어넣지만 **`_has_payload`를
다시 `true`로 되돌리지 않았다.**

결함이 있던 helper:

- `restore_part_from_native()`
- `restore_parts_from_native()` (vector / raw array 2종)
- `assign_parts_from_native()` (vector / raw array 2종)
- `take_parts_from_native()`
- `move_parts_to_native()`의 rollback 루프(같은 코드를 inline 복제하고 있었다)

결과: **실제 native payload를 가진 `message_t`가 binding metadata 상으로는 empty로
광고된다.**

### 2.2 왜 위험한가

C4가 `_has_payload`를 receive fast path의 **분기 선택자**로 쓴다
([`detail.hpp:94`](../../../../../bindings/cpp/src/Runtime/Sockets/detail.hpp#L94)).

```
if (part_out_.valid () && !has_payload (part_out_)) {
    // fast path: guard 없음, save/restore 없음
    rc = zlink_recv_part (...);
    if (rc != 0) { part_out_.close (); return rc; }   // <-- payload 파괴
    ...
}
```

즉 복구된(그러나 flag가 틀린) message를 다시 `recv()`에 넣으면 fast path가 선택되고,
receive가 실패하면 `close()`가 **살아 있는 payload를 파괴**한다. 이것은 "비어 있지
않은 출력 message는 receive 실패 시 보존한다"는 공개 계약 위반이다.

## 3. 수정

`bindings/cpp/src/Runtime/Native/native_message_parts.hpp` **한 파일**만 바꿨다.
Sol의 권고대로 중앙화했다.

```cpp
inline bool adopt_native_part (message_t &part_, zlink_msg_t &native_) noexcept
{
    if (!part_.valid ())
        return false;
    if (zlink_msg_move (detail::native_handle (part_), &native_) != 0)
        return false;
    detail::refresh_payload_presence (part_);   // empty => false, payload => true
    return true;
}

inline void restore_part_from_native (message_t &part_, zlink_msg_t &native_) noexcept
{
    part_.init ();
    (void) adopt_native_part (part_, native_);
    (void) zlink_msg_close (&native_);
}
```

- native payload를 `message_t`로 옮기는 **모든** 경로가 이 두 함수를 통과한다.
  수정 후 `src/` 전체에서 `zlink_msg_move` 직접 호출은 `adopt_native_part()`
  안의 1곳뿐이다.
- `restore_parts_from_native()` 2종, `move_parts_to_native()` rollback은
  inline 복제 코드를 지우고 `restore_part_from_native()` 호출로 바꿨다 —
  중복 제거가 곧 결함 재발 방지다.
- `assign_parts_from_native()` 2종과 `take_parts_from_native()`는
  `zlink_msg_move()` 직접 호출을 `adopt_native_part()`로 교체했다.
- 추가 `zlink_msg_size()`는 `refresh_payload_presence()` 안에서만 발생하며,
  이 경로들은 **send 실패 복구 / result materialization** 전용이다. C4의 hot
  single-part receive fast path는 건드리지 않았다.
- ownership, public API signature, error 동작 모두 불변. `assign_parts_from_native()`의
  실패 조건에 `!part_.valid()`가 추가되지만 `vector::resize()`가 만드는 message는
  항상 valid이므로 관측 가능한 동작 변화는 없다(오히려 invalid handle에 move 하던
  잠재 UB가 막힌다).

## 4. 검증

### 4.1 Sol의 A/B 목록 전용 프로그램

throwaway 검증 프로그램을 작성해 수정 **전/후 바이너리로 각각** 실행했다
(수정 전 헤더는 `git show HEAD:...`로 꺼내 `-I` 우선순위로 shadow).

| # | 시나리오 | 수정 전 | 수정 후 |
|---|----------|---------|---------|
| A1 | partial multipart send 실패 후 3개 part의 `valid()`/`size()`/`is_empty()`/`has_payload` | payload part 2개가 `has_payload=false` (**실패 2건**) | 전부 정확 (empty part는 `false`, payload part는 `true`) |
| A1b | `vector<zlink_msg_t>` 복구 + `move_parts_to_native()` rollback | `has_payload=false` (**실패 3건**) | 전부 정확 |
| A2 | **복구된 message를 다시 `recv()`에 투입** (수신 없음 → 실패) | `has_payload=false` → fast path 진입 → **`valid()=false`, payload 소멸** (**실패 5건**) | `valid()=true`, `size()=17`, 내용 보존 |
| A3 | `take_parts_from_native()` / `assign_parts_from_native()` 2종 materialization + materialize된 message의 실패 recv | `has_payload=false`, materialize된 message도 실패 recv에서 소멸 (**실패 8건**) | 전부 정확 |
| A4 | PAIR round trip 정상 경로(payload → payload 재사용 → empty frame) | 통과 | 통과 |

- 수정 전: **18건 실패**
- 수정 후: **0건 실패 (PASS)**

A2가 Sol이 지적한 바로 그 결함이며, 단순히 flag가 틀린 것이 아니라 **payload가
실제로 파괴되는 것**을 재현했다.

### 4.2 회귀 테스트

| 항목 | 결과 | 비고 |
|------|------|------|
| `bindings/cpp/tests/run_tests.sh` contract | **12 pass / 2 fail (14)** | 기존과 동일. 실패는 `test_cpp_contract_socket`(`submit_error_t`, `errno=113 EHOSTUNREACH`)과 `test_cpp_contract_request_reply`(`:714` 타이밍 assertion) — 둘 다 pre-existing |
| `bindings/cpp/samples/run_samples.sh` | **6 pass / 1 fail (7)** | 기존과 동일. `sample_cpp_dealer_router_recv_sample` pre-existing |
| ASan/UBSan contract (`build-sanitizers`) | **12 pass / 2 fail (14)** | Release와 **동일한 실패 집합**. `-fsanitize=address,undefined`, `detect_leaks=1`. **sanitizer 보고 0건** (AddressSanitizer/UndefinedBehaviorSanitizer/LeakSanitizer 출력 없음) |

ASan 빌드는 기존 `bindings/cpp/build-sanitizers` 캐시가 core 0.10.1을 가리키고
있어 현재 `core/build`로 재구성해 다시 빌드했다.

### 4.3 정상 경로 성능 회귀 (공식 조건, 64B/65536B만)

`--core-version 0.12.0`(release), duration 5, runs 3, C 먼저 실행, session tag
`c6-followup-20260823`.

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh --core-version 0.12.0 \
  --pattern PAIR --transports tcp --msg-sizes 64,65536 --duration 5 --runs 3 \
  --results-tag c6-followup-20260823

PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh --core-version 0.12.0 \
  --pattern PAIR --transports tcp --msg-sizes 64,65536 --duration 5 --runs 3 \
  --results-tag c6-followup-20260823
```

두 report 모두 `status: complete`, 10/10 result lines.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260823_135504_c6-followup-20260823.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260823_135549_c6-followup-20260823.txt`

| Size | C median throughput | C++ median throughput | throughput 비율 | (참고) 공식 §9.1.1 비율 | C median latency | C++ median latency | latency 비율 |
|-----:|--------------------:|----------------------:|----------------:|------------------------:|-----------------:|-------------------:|-------------:|
| 64B | 2,622,760.400 | 2,108,279.400 | **80.38%** | 80.93% | 52.946 ms | 62.854 ms | **1.187배** |
| 65536B | 45,868.400 | 40,463.000 | **88.22%** | 70.90% | 3.355 ms | 3.897 ms | **1.161배** |

**판정: C6로 인한 회귀 없음.**

- 64B는 80.38% vs 80.93%로 사실상 동일(0.55%p 차이는 잡음 범위).
- 65536B는 88.22%로 공식 70.90%보다 **17.32%p 높다.** 이것은 C6가 65536B를
  개선했다는 뜻이 **아니다** — C6는 정상 PAIR single-part 경로를 전혀 지나지
  않는다. 이 셀의 **run-to-run 분산이 매우 크다**는 뜻이다(§6 참조).

> **주의**: 이 두 셀은 계획서 §9.1.1의 전체 행을 대체하지 않는다.
> **C6-후속 확인**용 부분 재측정으로만 기록한다. 6개 size 전체 재측정은 별도
> 세션에서 수행한다.

---

# TASK B — C8: 64KiB 차분 진단

## 5. 방법

`bindings/cpp/perf/single/src/perf_pair.cpp`를 `/tmp/claude-1000` 아래로 복사해
throwaway 변형을 만들고, **컴파일 타임 매크로로 send/recv 계층만 C API 직접 호출로
치환**했다. 하네스(스레드 구성, poller 대기, stamp/decode, latency 누적, deadline
판정, stop token 처리)는 모든 변형에서 문자 그대로 동일하다.

| 변형 | send 경로 | recv 경로 |
|------|-----------|-----------|
| `base` | C++ binding (`socket.send().message(msg).flags(none).submit()`) | C++ binding (`socket.recv(message_t&, dontwait)`) |
| `csend` | C API (`zlink_msg_init_size`+`memcpy`+`zlink_send_part`) | C++ binding |
| `crecv` | C++ binding | C API (`zlink_msg_init`+`zlink_recv_part`+`zlink_msg_close`) |
| `cboth` | C API | C API |
| `cref` | — C reference 바이너리 `bindings/c/build-release-0.12.0/perf/perf_pair` — | |

- 네 C++ 변형 모두 `-O3 -DNDEBUG -std=gnu++20`, `bindings/cpp/build/libzlink_cpp.a`,
  release core `0.12.0/linux-x64/lib/libzlink.so`에 링크했다.
- `cref`도 `ldd`로 release `0.12.0` core를 쓰는 것을 확인했다
  (`bindings/c/build/perf/perf_pair`는 local core를 RPATH로 물고 있어 쓰지 않았다).
- 실행: `LD_LIBRARY_PATH=<release core>/lib PERF_SINGLE_DURATION_SECONDS=5 ./pair_<변형> current tcp <size>`
- 크기 5종 × 변형 5종 × 3회, 같은 셸 세션에서 `cref → base → csend → crecv → cboth`
  순서로 인터리브 실행(드리프트를 특정 변형에 몰아주지 않기 위해).

## 6. 결과

### 6.1 계층별 차분 (5초, 3회 median, msg/s)

| Size | `cref` | `base` | `csend` | `crecv` | `cboth` | `base`/`cref` | `cboth`/`cref` | **binding 계층이 설명하는 gap 비중** |
|-----:|-------:|-------:|--------:|--------:|--------:|--------------:|---------------:|--------------------------------------:|
| 32768 | 74,474 | 62,711 | 60,792 | 57,801 | 60,262 | 84.21% | 80.92% | **−20.8%** (치환이 오히려 느려짐) |
| 65535 | 45,517 | 32,053 | 31,874 | 32,542 | 31,862 | 70.42% | 70.00% | **−1.4%** |
| 65536 | 45,741 | 32,059 | 31,946 | 36,798 | 37,943 | 70.09% | 82.95% | +43.0% (§6.3의 bimodal 잡음, 아래 참조) |
| 65537 | 47,184 | 33,118 | 32,237 | 31,789 | 37,617 | 70.19% | 79.73% | +32.0% (동일) |
| 131072 | 26,466 | 21,993 | 21,778 | 21,581 | 22,413 | 83.10% | 84.69% | **+9.4%** |

"binding 계층이 설명하는 gap 비중" = (`cboth` − `base`) / (`cref` − `base`).
즉 **send·recv 두 계층을 모두 C API 직접 호출로 치환했을 때 회복되는 처리량이
전체 gap에서 차지하는 비율**이다.

### 6.1.1 65536B 부근 pooled 통계 (65535·65536·65537 = 변형당 9회)

65536/65537의 +43%/+32%는 계층 효과가 아니라 **bimodal 잡음**이다. 원시값(§6.2)을
보면 `base`를 포함한 **모든** C++ 변형이 약 32k 모드와 약 38k 모드 사이를
오간다. 세 크기를 pooled 하면 다음과 같다.

| 변형 | min | median | max | mean | CV |
|------|----:|-------:|----:|-----:|---:|
| `cref` | 44,701 | 46,335 | 47,392 | 46,158 | **1.9%** |
| `base` | 31,894 | 32,059 | 37,415 | 32,896 | 5.4% |
| `csend` | 31,273 | 31,946 | 39,342 | 33,423 | 9.5% |
| `crecv` | 31,189 | 32,237 | 37,480 | 33,186 | 7.0% |
| `cboth` | 31,748 | 33,099 | 38,285 | 34,805 | 8.7% |

- 네 C++ 변형의 분포는 **서로 완전히 겹친다**(모두 min ≈ 31.2–31.9k, max ≈ 37.4–39.3k).
- `cref` 분포는 네 변형 중 **어느 것과도 겹치지 않는다**(min 44,701 > 모든 C++ max).
- pooled mean 기준 회복량: `csend` **4.0%**, `crecv` **2.2%**, `cboth` **14.4%**
  — 그리고 이 14.4%는 `base` 자신의 CV(5.4%)와 `cboth`의 CV(8.7%) 안에 들어간다.

### 6.1.2 65536 경계에서 계단이 있는가 — **없다**

| Size | `base`/`cref` |
|-----:|--------------:|
| 32768 | 84.21% |
| 65535 | **70.42%** |
| 65536 | **70.09%** |
| 65537 | **70.19%** |
| 131072 | 83.10% |

65535 → 65536 → 65537에서 비율은 **0.33%p 안에서 평평하다.** 65536에서의 계단은
**존재하지 않는다.** 저하는 32768(84.21%)과 65535(70.42%) 사이에서 완만하게
일어나고 131072(83.10%)에서 회복되는 **넓은 골(dip)**이며, 어떤 크기 상수의
경계 효과도 아니다. 이는 C9(pool 하한 65536B) 가설의 전제를 직접 부정한다.

### 6.1.3 page fault / context switch (65536B, `/usr/bin/time -v`)

| 대상 | throughput | minor page faults | voluntary ctx switch | system time |
|------|-----------:|------------------:|---------------------:|------------:|
| `cref` run1/2/3 | 47,703 / 46,517 / 48,124 | **1,675 / 1,533 / 1,694** | 163,165 / 153,480 / 162,433 | 4.64 / 4.81 / 4.70 s |
| `base` run1/2/3 | 33,004 / 32,739 / 32,604 | **1,282,435 / 1,353,629 / 1,299,515** | 135,076 / 133,317 / 132,773 | 5.90 / 5.97 / 5.89 s |
| `csend` | 39,049 | 1,350,807 | 138,911 | 6.10 s |
| `crecv` | 33,294 | 1,330,025 | 138,223 | 5.94 s |
| `cboth` | 39,116 | **1,402,120** | 135,851 | 5.97 s |

**C++ 프로세스는 C 대비 minor page fault가 약 800배 많다** (약 1.3M vs 약 1.6k).
메시지당 약 8회다.

결정적으로 **`cboth`도 똑같이 1.4M**이다. `cboth`의 hot loop에는 binding
send/recv 호출이 하나도 없다. 따라서 이 page fault storm은 **binding 호출
계층의 산물이 아니다.**

### 6.1.4 원인 규명 — glibc heap trim

가설을 환경변수로 직접 검증했다(코드 변경 없음).

| 실행 | throughput | minor page faults |
|------|-----------:|------------------:|
| `base` 기본 | 39,861 / 32,570 / 38,904 | 1,286,166 / 1,323,658 / 1,358,182 |
| `base` + `MALLOC_TRIM_THRESHOLD_=134217728` | **47,830 / 47,909 / 49,671** | **1,599 / 1,744 / 1,856** |
| `cref` 기본 | 45,268 / 46,929 / 46,914 | 1,633 / 1,570 / 1,618 |
| `cref` + `MALLOC_MMAP_THRESHOLD_=1048576` | **41,178** | **1,300,877** |

- `base`에 glibc heap trim만 끄면 page fault가 **1.3M → 1.7k로 사라지고**,
  throughput이 32~40k에서 **47.8~49.7k로 올라 `cref`(45.3~46.9k)를 넘어선다.**
  이 조건에서 C++/C 비율은 median 기준 47,909 / 46,914 = **102.1%**다.
- 반대로 `cref`에 `MALLOC_MMAP_THRESHOLD_`를 명시 지정해 glibc의 **동적**
  mmap/trim threshold 적응을 꺼버리면, **C reference도 똑같이 1.3M page fault에
  빠지고 41.2k로 떨어진다.**

즉 메커니즘은 다음과 같다. 64KiB payload 할당은 glibc 기본 `M_MMAP_THRESHOLD`
(128KiB) **바로 아래**라 heap(brk)에서 할당된다. glibc는 free 시
`M_TRIM_THRESHOLD`에 따라 heap 꼭대기를 커널에 되돌려주고, 다음 메시지에서 같은
영역을 다시 touch 하면서 16 page를 재폴트한다. 어느 프로세스가 이 루프에
빠지는지는 glibc의 **동적 threshold 적응이 언제 발동했는가**라는
프로세스 할당 이력에 달려 있고, C와 C++ 어느 쪽이든 빠질 수 있다 — 위 표의
마지막 행이 그 증거다.

이것은 **binding library의 비용이 아니라 프로세스 allocator 튜닝(환경)의
산물**이다.

### 6.2 원시 반복값

| Size | 변형 | run1 | run2 | run3 | median |
|-----:|------|-----:|-----:|-----:|-------:|
| 32768 | `cref` | 73,281 | 74,474 | 74,740 | **74,474** |
| 32768 | `base` | 62,711 | 59,130 | 62,812 | **62,711** |
| 32768 | `csend` | 60,792 | 64,111 | 60,625 | **60,792** |
| 32768 | `crecv` | 57,292 | 57,801 | 61,386 | **57,801** |
| 32768 | `cboth` | 56,422 | 60,262 | 66,990 | **60,262** |
| 65535 | `cref` | 44,701 | 46,614 | 45,517 | **45,517** |
| 65535 | `base` | 32,439 | 32,053 | 31,894 | **32,053** |
| 65535 | `csend` | 31,874 | 32,267 | 31,399 | **31,874** |
| 65535 | `crecv` | 33,045 | 32,542 | 32,237 | **32,542** |
| 65535 | `cboth` | 31,862 | 31,748 | 33,099 | **31,862** |
| 65536 | `cref` | 45,404 | 45,741 | 46,335 | **45,741** |
| 65536 | `base` | 32,059 | 32,028 | 33,138 | **32,059** |
| 65536 | `csend` | 31,946 | 38,657 | 31,811 | **31,946** |
| 65536 | `crecv` | 31,189 | 36,798 | 37,480 | **36,798** |
| 65536 | `cboth` | 32,540 | 38,037 | 37,943 | **37,943** |
| 65537 | `cref` | 47,184 | 46,532 | 47,392 | **47,184** |
| 65537 | `base` | 31,921 | 33,118 | 37,415 | **33,118** |
| 65537 | `csend` | 39,342 | 31,273 | 32,237 | **32,237** |
| 65537 | `crecv` | 31,789 | 31,706 | 31,888 | **31,789** |
| 65537 | `cboth` | 38,285 | 32,114 | 37,617 | **37,617** |
| 131072 | `cref` | 26,466 | 26,229 | 27,604 | **26,466** |
| 131072 | `base` | 22,195 | 21,993 | 21,652 | **21,993** |
| 131072 | `csend` | 21,778 | 21,563 | 23,422 | **21,778** |
| 131072 | `crecv` | 21,635 | 21,504 | 21,581 | **21,581** |
| 131072 | `cboth` | 22,369 | 23,030 | 22,413 | **22,413** |

## 7. 결론

### 7.1 64KiB gap 중 binding 호출 계층의 몫

**거의 0이다.**

| 근거 | 값 |
|------|-----|
| 65535B에서 send+recv 두 계층 모두 C API로 치환했을 때 회복량 | **−1.4%** (오히려 감소) |
| 32768B 동일 치환 | **−20.8%** |
| 131072B 동일 치환 | **+9.4%** |
| 65535·65536·65537 pooled mean 기준 `cboth` 회복량 | **+14.4%**, 단 `base` 자체 CV 5.4% / `cboth` CV 8.7% 안 |
| 네 C++ 변형의 처리량 분포 | **완전히 겹침** (min 31.2–31.9k, max 37.4–39.3k) |
| `cref` 분포 | 네 변형 **어느 것과도 겹치지 않음** (min 44,701 > 모든 C++ max) |

공식 70.90% gap 중 **binding send/recv 호출 계층으로 귀속되는 몫은 측정 잡음과
구분되지 않는다.** 64B에서 binding 계층이 gap의 약 절반을 차지했던 것
(`log/2026-08-23-cpp-pair-tcp-profile.md` §4.2)과 정반대다. 크기가 커지면
호출당 고정 wrapper 비용이 payload 처리 시간에 묻히므로 이는 예상과 부합한다.

### 7.2 그러면 무엇인가 — 환경(프로세스 allocator)

§6.1.3–§6.1.4가 원인을 특정했다.

- C++ 프로세스는 65536B 셀에서 minor page fault를 **약 1.3M회**(메시지당 약 8회)
  일으키고, C reference는 **약 1.6k회**만 일으킨다.
- 이 storm은 hot loop에 binding 호출이 하나도 없는 `cboth`에서도 **동일하게**
  발생한다 → binding 코드가 원인이 아니다.
- `base`에서 glibc heap trim만 끄면 page fault가 1.3M → 1.7k로 사라지고
  throughput이 **47.8~49.7k**로 올라 `cref`(45.3~46.9k)를 **넘어선다**
  (C++/C 비율 **102.1%**).
- 반대로 `cref`에서 glibc의 동적 threshold 적응을 꺼면 **C reference도** 1.3M
  page fault에 빠지며 41.2k로 떨어진다.

메커니즘은 64KiB payload가 glibc 기본 `M_MMAP_THRESHOLD`(128KiB) 바로 아래에
있어 brk heap에서 할당되고, free 시 `M_TRIM_THRESHOLD`에 의해 heap 꼭대기가
커널에 반환됐다가 다음 메시지에서 16 page를 재폴트하는 것이다. 어느 프로세스가
이 루프에 걸리는지는 glibc 동적 threshold 적응이 언제 발동했는지에 달려 있고
**C·C++ 어느 쪽이든 걸릴 수 있다.**

### 7.3 65536 경계 계단 — 없다

`base`/`cref` 비율은 65535 / 65536 / 65537에서 **70.42% / 70.09% / 70.19%**로
0.33%p 안에서 평평하다. 저하는 32768과 65535 사이의 완만한 골이며 131072에서
회복된다. **정확히 65536에서 일어나는 계단은 존재하지 않는다.**

이는 C9(large-message pool 하한을 65536B로 확장)의 전제를 직접 부정한다.
공략할 경계 자체가 없다.

### 7.4 gap 위치 분류 (Sol이 요구한 형태)

| 분류 | 65536B gap에서의 몫 | 근거 |
|------|--------------------|------|
| **binding call layer** (send/recv wrapper, `message_t::from()`, `zlink_msg_size()`) | **≈ 0%** (잡음과 구분 불가) | `cboth` − `base` = −1.4% ~ +14.4%, 분포 완전 중첩 |
| **Core / transport** | **≈ 0%** | `cboth`와 `cref`는 동일한 Core API 호출 시퀀스를 쓰는데도 여전히 차이가 남 → Core 호출 자체가 아님 |
| **환경 / 프로세스 allocator** | **거의 전부** | glibc heap trim 무력화만으로 C++가 C를 상회(102.1%). 같은 조건을 C에 강제하면 C도 동일하게 저하 |

### 7.5 권고

1. **C8은 완료. 새 후보 없음.** 65536B에 대한 binding-only source 최적화 후보를
   설계할 근거가 없다. Sol의 조건("binding-only 차이가 없으면 Core/transport
   영역으로 분류하고 후보를 만들지 않는다")에 정확히 해당한다.
2. **C9는 no-go 확정.** 공략 대상인 65536 경계 계단이 존재하지 않는다.
3. **binding library에서 `mallopt()`를 호출하지 않는다.** 프로세스 전역
   allocator 정책은 라이브러리가 소유할 책임이 아니다(application의 책임).
   이것은 POSDDD 상 명백한 책임 위반이며, 측정 수치를 위해 도입해서는 안 된다.
4. **perf 하네스에도 allocator 튜닝을 넣지 않는다.** C reference가 측정 계약을
   소유하므로(계획서 §5, C5 판정과 동일 논리) C++ 하네스만 `MALLOC_*`를 설정하면
   측정 의미가 갈라진다.
5. **공식 65536B 셀 수치의 해석을 기록한다.** 공식 70.90%는 binding 비용이
   아니라 대부분 glibc heap-trim 아티팩트다. 이번 재측정에서 같은 셀이
   **88.22%**로 나온 것이 그 분산의 크기를 보여준다. §9.1.1 판정 시 이 셀을
   binding 결함의 증거로 사용해서는 안 된다.
6. **PAIR/tcp 2차 개선 패스는 `보류 확정`.** C2·C9·C10 모두 no-go이고, C8은
   후보를 만들지 않는다는 결론을 냈다. 남은 것은 C7(TLS state pool
   micro-A/B, 64B 전용 저위험 실험)뿐이며 이는 aggregate 5.08%p를 회복할
   후보가 아니다.

## 8. 정리

### 8.1 저장소에 남긴 변경

| 파일 | 종류 |
|------|------|
| `bindings/cpp/src/Runtime/Native/native_message_parts.hpp` | **shipping code — C6 수정** (유일한 소스 변경) |
| `doc/perf/perf/bindings-0.12.0/log/2026-08-23-c6-fix-and-64k-diagnosis.md` | 이 문서 |
| `doc/perf/perf/bindings-0.12.0/progress.ko.md` §5 | C6 행 추가, C2 no-go 확정, C9·C10 no-go 행 추가 |
| `bindings/c/perf/results/single/report/perf_c_single_linux_20260823_135504_c6-followup-20260823.txt` | 공식 재측정 결과물 |
| `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260823_135549_c6-followup-20260823.txt` | 공식 재측정 결과물 |

TASK B는 **shipping code를 전혀 변경하지 않았다.** commit·push도 하지 않았다.

### 8.2 삭제한 throwaway 산출물

모두 `/tmp/claude-1000` 아래에만 만들었고 작업 종료 시점에 삭제했다.

- C6 검증 프로그램(`c6_verify.cpp`)과 수정 전/후 바이너리 2개, 수정 전 헤더
  shadow 디렉터리
- ablation 변형 소스 `perf_pair_variant.cpp`, 실제 트리를 가리키던 symlink
  (`perf/common`, `perf/single/common`), 바이너리 4개
  (`pair_base`/`pair_csend`/`pair_crecv`/`pair_cboth`), sweep 스크립트와 CSV,
  page-fault 측정 스크립트

`git status --short`로 이번 작업 전후의 변경 파일 목록이 위 표 외에는 동일함을
확인했다. 트리에 계측 코드는 남아 있지 않다.

### 8.3 재구성한 빌드 디렉터리

`bindings/cpp/build-sanitizers`는 기존 캐시가 core 0.10.1을 가리키고 있어
현재 `core/build`로 재구성했다(빌드 산출물 디렉터리이며 트리에 커밋되지 않는다).

```bash
cmake -S bindings/cpp -B bindings/cpp/build-sanitizers \
  -DCMAKE_BUILD_TYPE=Debug \
  -DZLINK_CORE_DIR=.../core -DZLINK_CPP_CORE_BUILD_DIR=.../core/build \
  -DZLINK_CPP_BUILD_TESTS=ON -DZLINK_CPP_BUILD_SAMPLES=OFF -DZLINK_CPP_BUILD_BENCHMARKS=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
```
