# C++ `PUBSUB`/`tcp` 자체 개선 pass (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> §5(고정 원칙), §7.2(반복 횟수), §7.3(paired C 규칙), §8(판정), §9.1.1(Single suite 표)
>
> 선행 기록: `log/2026-08-23-cpp-pubsub-tcp-official.md`(첫 공식 측정, aggregate 89.40% 미달),
> `log/2026-08-23-c6-fix-and-64k-diagnosis.md`(`PAIR` 65536B의 glibc heap-trim 진단),
> `log/2026-08-23-cpp-pair-tcp-profile.md`(경로 치환 차분 ablation 방법론)
>
> 이 문서는 `PUBSUB`/`tcp` 전용 자체 개선 pass의 진단·후보 검토·구현·재측정 기록이다.
> commit·push는 하지 않았다.

## 1. 환경 manifest

| 항목 | 값 |
|------|-----|
| host | `ulalax-gram` |
| OS/kernel | `Linux 6.6.87.2-microsoft-standard-WSL2` (WSL2) |
| CPU | `x86_64`, 16 logical cores (`12th Gen Intel(R) Core(TM) i7-1260P`) |
| memory | 11Gi total |
| 작업 브랜치 / commit | `codex/bindings-0.12.0-performance` / `f99703c2190b0f6c670be49f67315d904886c742`(HEAD, 미커밋 변경 포함) |
| Core runtime | release `0.12.0`, prefix `/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64`, `lib/libzlink.so.0.12.0` |
| Core release tag / provenance revision | `core/v0.12.0` / `f99703c2190b0f6c670be49f67315d904886c742`, `dirty: false` |
| C reference 바이너리 | `bindings/c/build-release-0.12.0/perf/perf_pubsub` (`ldd`로 위 release core 사용 확인) |
| session tag (재측정) | `bindings-0.12.0-official-pubsub-pass2-20260823` |
| 시각 | 2026-08-23 14:25 ~ 14:53 KST |

첫 공식 측정(§9.1.1 기존 행)의 size별 비율: 64B 87.94%, 256B 93.75%, 1024B 93.19%,
**65536B 78.29%**, 131072B 90.04%, 262144B 93.16% — aggregate throughput mean 89.40%,
aggregate latency mean 1.062배.

---

# TASK A — 65536B 셀 환경 판정

## 2. 방법

`PAIR`의 65536B 저하가 binding 결함이 아니라 glibc heap-trim page fault storm이라는
`log/2026-08-23-c6-fix-and-64k-diagnosis.md` §6.1.3~§6.1.4의 진단을 `PUBSUB`에도 그대로
적용했다. 공식 조건(release core, 5초, `PUBSUB`/`tcp`/65536B)에서 C reference 바이너리와
C++ 공식 perf 바이너리를 **같은 셸 세션에서 인터리브**(`cref → cpp` × 3회)로 실행하고
`/usr/bin/time -v`로 fault·context switch·CPU 시간을 함께 기록했다.

```bash
CORE=/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64/lib
LD_LIBRARY_PATH=$CORE PERF_SINGLE_DURATION_SECONDS=5 /usr/bin/time -v \
  ./bindings/c/build-release-0.12.0/perf/perf_pubsub c tcp 65536
LD_LIBRARY_PATH=$CORE PERF_SINGLE_DURATION_SECONDS=5 /usr/bin/time -v \
  ./bindings/cpp/perf/single/build/cpp_perf_pubsub cpp tcp 65536
```

## 3. 결과 — `PAIR`와 동일한 비대칭 fault 서명

### 3.1 기본 실행 (인터리브 3회)

| 대상 | throughput (msg/s) | minor page faults | major | voluntary ctx switch | system time |
|------|-------------------:|------------------:|------:|---------------------:|------------:|
| C run1/2/3 | 47,246.0 / 50,999.2 / 48,225.0 | **2,185 / 2,147 / 1,986** | 0 | 110,051 / 117,732 / 110,440 | 4.73 / 4.54 / 4.65 s |
| C++ run1/2/3 | 41,687.8 / 42,879.4 / 41,810.8 | **1,161,283 / 1,128,339 / 1,360,598** | 0 | 107,175 / 107,650 / 106,354 | 5.74 / 5.88 / 5.90 s |

median: C 48,225.0 msg/s / minor 2,147, C++ 41,810.8 msg/s / minor 1,161,283.
**C++ 프로세스의 minor page fault가 C의 약 540배**(메시지당 약 5.6회)이고,
system time만 약 1.2초 더 크다. voluntary context switch는 두 쪽이 거의 같다
(약 11만 회) — 즉 I/O 대기 구조가 아니라 **페이지 폴트**가 차이의 위치다.
`PAIR` 65536B에서 관측된 서명(C++ 약 1.3M vs C 약 1.6k)과 같은 종류다.

### 3.2 원인 확인 — 양방향 검증 (코드 변경 없음, 환경변수만)

| 실행 | throughput (msg/s) | minor page faults |
|------|-------------------:|------------------:|
| C++ 기본 | 41,687.8 / 42,879.4 / 41,810.8 | 1,161,283 / 1,128,339 / 1,360,598 |
| **C++ + `MALLOC_TRIM_THRESHOLD_=134217728`** | **45,481.0 / 49,866.8 / 40,985.6** | **173,884 / 7,131 / 428,435** |
| C 기본 | 47,246.0 / 50,999.2 / 48,225.0 | 2,185 / 2,147 / 1,986 |
| **C + `MALLOC_MMAP_THRESHOLD_=1048576`** | **34,071.6 / 34,214.6 / 34,341.0** | **1,051,751 / 1,311,397 / 1,126,023** |

- C++에 glibc heap trim만 끄면 fault가 1.16M → 173k(최소 7.1k)로 붕괴하고 처리량이
  median 41,810.8 → **45,481.0**으로 오른다. 이 조건에서 C++/C = 45,481.0 / 48,225.0 =
  **94.31%**다(공식 셀 70.18% 대비 +24%p).
- 반대로 C에 glibc의 **동적** mmap/trim threshold 적응을 꺼버리면 **C reference도 똑같이
  1.05~1.31M fault에 빠지고 34.2k로 무너진다.** 이 상태의 C(34,214.6)는 C++의 기본
  실행(41,810.8)보다 **느리다** — 같은 fault 체제에 놓으면 C++/C = **122.20%**다.

메커니즘은 `PAIR` 진단과 동일하다. 64 KiB payload 할당은 glibc 기본
`M_MMAP_THRESHOLD`(128 KiB) 바로 아래라 heap(brk)에서 할당되고, free 시
`M_TRIM_THRESHOLD`에 따라 heap 꼭대기가 커널로 반환됐다가 다음 메시지에서 16 page를
재폴트한다. 어느 프로세스가 이 루프에 빠지는지는 **glibc 동적 threshold 적응이 언제
발동했는가**라는 프로세스 할당 이력의 문제이고, C와 C++ 어느 쪽이든 빠질 수 있다.

## 4. 판정과 기록용 계산

**`PUBSUB`/`tcp` 65536B 셀도 `PAIR`와 같은 환경 지배 셀로 판정한다.** binding 결함의
증거로 사용하지 않는다. 근거는 (a) C++의 minor fault가 C의 약 540배라는 비대칭,
(b) trim을 끄면 C++가 94.31%까지 회복, (c) 같은 조건을 C에 강제하면 C가 C++보다 낮아지는
역전 — 세 가지다.

**기록용(판정 입력 아님)**: 재측정(§8) 기준으로 65536B 셀을 제외한 5개 셀의 aggregate
throughput mean은 **89.34%**다(첫 공식 측정 기준으로는 91.62%). 계획서 §8은 측정된 모든
size의 산술평균으로 판정하므로 **이 값은 판정에 사용하지 않는다.**

`MALLOC_*` 튜닝은 라이브러리에도 perf 하네스에도 넣지 않는다. 프로세스 전역 allocator
정책은 application의 책임이며(POSDDD 책임 경계 위반), C reference가 측정 계약을 소유하므로
C++ 하네스만 `MALLOC_*`를 설정하면 측정 의미가 갈라진다(계획서 §5, `PAIR` C5·C8 판정과
동일 논리).

---

# TASK B — 64B binding overhead ablation

## 5. 방법 — 경로 치환 차분

`log/2026-08-23-cpp-pair-tcp-profile.md` §3과 같은 방법이다.
`bindings/cpp/perf/single/src/perf_pubsub.cpp`를 scratchpad로 복사해 throwaway 변형을
만들고, **컴파일 타임 매크로로 publish/subscribe 계층만 C API 직접 호출로 치환**했다.
하네스(스레드 구성, poller 대기, stamp/decode, latency 누적, deadline 판정, stop token)는
모든 변형에서 문자 그대로 동일하다. 트리 안의 코드는 이 단계에서 건드리지 않았다.

| 변형 | publish 경로 | subscribe 경로 |
|------|--------------|----------------|
| `official` | 트리의 공식 `cpp_perf_pubsub` 바이너리 (변형 없음) | 동일 |
| `base` | C++ binding (`publisher.publish(topic).message(msg).submit()`) | C++ binding (`subscriber.subscribe(topic_message_t&, dontwait)`) |
| `cpub` | C API (`zlink_msg_init_size`+`memcpy`+`zlink_publish_part`) | C++ binding |
| `csub` | C++ binding | C API (`zlink_msg_init`+`zlink_subscribe_part`+`zlink_msg_close`) |
| `cboth` | C API | C API |
| `mid` | **`message_t::from()`은 유지하고 builder만 우회** (`zlink_publish_part`에 `native_handle(msg)` 직접 전달 + `mark_sent`) | C++ binding |
| `norecalc` | `base`에서 `recalculate_single_auto_hwm()` 호출만 제거 | C++ binding |
| `cref` | — C reference 바이너리 `bindings/c/build-release-0.12.0/perf/perf_pubsub` — | |

빌드: `-O3 -DNDEBUG -std=gnu++20`(트리의 perf CMake와 동일 플래그), 트리의
`bindings/cpp/build/libzlink_cpp.a`와 이미 빌드된 perf common 오브젝트, release core
`0.12.0` `libzlink.so`에 링크. 실행:
`LD_LIBRARY_PATH=<release core>/lib PERF_SINGLE_DURATION_SECONDS=5 ./pubsub_<변형> cpp tcp 64`.
드리프트를 특정 변형에 몰아주지 않기 위해 매 반복마다 `cref → official → base → ...`
순서로 인터리브 실행했다.

## 6. 결과

### 6.1 계층별 차분 (64B, 5초, 8회 median)

| 변형 | median throughput (msg/s) | `cref` 대비 | `base` 대비 |
|------|--------------------------:|------------:|------------:|
| `cref` | 1,530,064 | 100.0% | — |
| `official` | 1,360,318 | 88.9% | −1.3% |
| `base` | 1,377,775 | 90.0% | — |
| `cpub` | 1,467,501 | 95.9% | **+89,726 (+6.5%)** |
| `csub` | 1,345,434 | 87.9% | **−32,341 (−2.3%)** |
| `cboth` | 1,445,431 | 94.5% | +67,656 (+4.9%) |
| `norecalc` | 1,359,630 | 88.9% | −18,145 (−1.3%) |

- `base` gap = `cref` − `base` = 152,289 msg/s.
- **publish 계층이 gap의 58.9%를 설명한다**(`cpub` − `base`).
- **subscribe 계층은 0이 아니라 음수다**(`csub` − `base` = −2.3%). C API로 치환하면
  오히려 느려진다 — binding의 `subscribe()`는 `topic_message_t`의 저장소를 재사용하고
  `lazy_message_parts_t`가 vector capacity를 유지하는 반면, 치환 변형은 매 수신마다
  `zlink_msg_init`/`zlink_msg_close`를 왕복한다. **recv 계층은 후보 대상이 아니다.**
- `norecalc`는 잡음 수준(−1.3%, 8회 중 6회 낮음)이다. 성능 후보가 아니라 측정 의미
  정렬 항목이다(§7 P1).

### 6.2 publish 계층 안의 분해 (64B, 5초, 5회 median)

| 변형 | median throughput (msg/s) | `cref` 대비 | `base` 대비 |
|------|--------------------------:|------------:|------------:|
| `cref` | 1,520,321 | 100.0% | — |
| `base` | 1,384,392 | 91.1% | — |
| **`mid`** (message_t 유지, builder 우회) | 1,493,047 | 98.2% | **+108,655 (+7.8%)** |
| `cpub` (message_t도 제거) | 1,518,766 | 99.9% | +134,374 (+9.7%) |

`base` gap = 135,929 msg/s. 그중
**`send_operation_t` builder + pooled `operation_state_t` 기계장치가 79.9%**(`mid`−`base`),
**`message_t::from()` 할당·소유권 계층이 나머지 18.9%**(`cpub`−`mid`)다.

### 6.3 publish 호출 자체의 CPU 비용 (throwaway 마이크로 계측)

네트워크를 배제하기 위해 subscriber 없는 inproc PUB 소켓에 200만 회 publish 하고
호출당 ns를 쟀다(3반복 median).

| 계측 | ns/call | 차분 |
|------|--------:|------|
| A `publish(topic).message(msg).submit()` (공개 builder) | 279.3 | — |
| B `message_t` + `zlink_publish_part` 직접 | 251.5 | A−B = **27.8 ns** (builder 계층) |
| C `zlink_msg_init_size`+`memcpy`+`zlink_publish_part` | 244.9 | B−C = 6.6 ns (`message_t` 계층) |
| D = B + 비경합 `std::mutex` lock/unlock 1회 | 255.2 | D−B = **3.7 ns** (submit의 `outbound_record_attempt_mutex` 몫) |
| E pooled `operation_state_t` acquire+reset+release만 | **4.2** | (pool 기계장치 몫) |

**27.8 ns의 내역**: mutex 3.7 ns + pooled state 4.2 ns + **나머지 약 19.9 ns**.
나머지는 어느 한 항목이 아니라 **fluent builder 공개 계약 자체의 비용**이다 —
`publish()` / `message()` / `submit()` 세 개의 인라인 불가 out-of-line 호출, 두 builder
객체를 지나가는 `unique_ptr` 이동, `validate_no_embedded_null`, topic 문자열 대입,
`socket_closed` atomic load 2회, `bind_callback_state`/`live_callback_state`의
`weak_ptr::expired()` 2회, `has_send_parts`와 kind switch 분기.

이 계측은 **C2 no-go 판정이 성능상 무해했음도 확인해 준다**(mutex는 3.7 ns로 27.8 ns 중
13%에 불과하다).

### 6.4 `PUBSUB` 고유 비용은 무엇인가 — 거의 없다

소스 비교 기준으로 `pub_socket_t::publish(topic)`은 `pair_socket_t::send()`에
**정확히 두 줄**을 더한 것이다.

| `PUBSUB` 고유 항목 | 위치 | 추정 비용 | 판단 |
|--------------------|------|-----------|------|
| topic 문자열 처리 (`raw.topic = topic_id_`, `reset()`의 `topic.clear()`, submit의 `topic.empty()`/`c_str()`) | `pubsub.cpp`, `operation_state.hpp`, `operation_submit.hpp` | 약 3~4 ns (topic `"bench"`는 SSO, malloc 없음) | 제거 불가(계약), 유의미하지 않음 |
| `validate_no_embedded_null(topic_id_, "topic")` | 헤더 inline, 5바이트 `find('\0')` | 약 2 ns | **계약(error semantics)** — 제거 불가 |
| subscription matching wrapper | `sub_socket_t::set_subscription()` | hot path 아님 — 설정 시 1회 | 해당 없음 |
| publish builder state | `raw_publish` kind는 `raw_send`와 같은 `operation_state_t`·같은 pool·같은 submit fast path를 쓴다 | `raw_send`와 동일 | **`PUBSUB` 고유 아님** |

**결론: 64B `PUBSUB` gap의 binding 몫은 `PAIR`에서 이미 분석·최적화된 공용 send builder
계층과 같은 것이다.** topic 처리는 publish 호출 CPU의 약 2%(27.8 ns 중 약 5 ns), 즉 전체
처리량의 0.1% 미만이고, subscription matching은 hot path에 없으며, recv 계층은 이득이
음수다. **`PUBSUB` 전용 성능 후보를 설계할 근거가 없다.**

---

# TASK C — 후보와 구현

## 7. 후보 순위와 판정

| 후보 | 대상 | 성격 | 판정 |
|------|------|------|------|
| **P1** `recalculate_single_auto_hwm()` 제거 | `bindings/cpp/perf/single/src/perf_pubsub.cpp` | 측정 의미 정렬 (C5의 `PUBSUB` 후속) | **구현함 (성능 중립~−1.3%)** |
| **P2** publish 조립·config 오류 변환 중복 제거 | `bindings/cpp/src/Runtime/Sockets/pubsub.cpp` | POSDDD 구조 개선 (중복 제거) | **구현함 (성능 중립)** |
| P3 builder 계층 자체 축소 | `send_operation_t` / `operation_builder_base_t` | 성능 개선 (약 20 ns/publish) | **no-go** — 공개 fluent builder 계약(`publish()`→`message()`→`submit()`)과 공개 builder type이 비용의 원천이다. 계획서 §5는 공개 interface·type 변경을 금지하고, C3에서 "공개 builder가 `operation_builder_base_t`의 `unique_ptr<operation_state_t>`를 보유하므로 스택 state 전환은 불가"로 이미 확정했다 |
| P4 topic 처리 최적화 | `pub_socket_t::publish` / `raw_command_t::topic` | 성능 개선 | **no-go** — 총 약 5 ns/publish(전체의 0.1% 미만)이고, `validate_no_embedded_null`은 계약(error semantics)이라 제거 불가 |
| P5 recv/subscribe 계층 축소 | `read_subscription_message` / `lazy_message_parts_t` | 성능 개선 | **no-go** — §6.1에서 binding recv가 C API 치환보다 **빠르다**(−2.3%). 회복할 gap이 없다 |
| P6 65536B 전용 최적화 | — | 성능 개선 | **no-go** — §4에서 환경(프로세스 allocator) 지배로 판정. `PAIR` C8·C9와 같은 결론 |
| P7 `MALLOC_*` 튜닝 도입 | 라이브러리 또는 하네스 | — | **no-go(확정)** — POSDDD 책임 경계 위반 / 측정 의미 분기. 이미 `PAIR` pass에서 판정됨 |

### 7.1 P1 — 구현 내용

C reference `bindings/c/perf/single/src/perf_pubsub.cpp`에는 context 수준 auto-HWM
재계산 호출이 **없다**(C single runner 전체에 `recalculate_auto_hwm` 호출이 없음).
C++ 러너에는 `PAIR`을 제외한 모든 pattern에 남아 있었다. C5가 `perf_pair.cpp`에서만
제거하고 "다른 pattern 파일은 건드리지 않아 hunk가 분리돼 있다"고 기록했으므로, 이번
`PUBSUB` pass에서 같은 정렬을 수행한다.

```diff
     (subscriber.*set_subscription) (std::string ());
-    if (!perf::single::recalculate_single_auto_hwm (ctx)) {
-        if (perf_debug_enabled ())
-            std::cerr << "pubsub: auto-hwm recalculation failed errno=" << errno << std::endl;
-        return false;
-    }
+    // PERF policy (plan 0.12.0 §5): the C reference runner
+    // bindings/c/perf/single/src/perf_pubsub.cpp never recalculates the
+    // context auto-HWM, so neither does this runner. ...
```

**중요**: 이 제거는 C++ 쪽에 유리하지 **않다**(§6.1의 `norecalc`가 `base`보다 −1.3%).
즉 지금까지 C++ 하네스가 C에 없는 단계를 하나 더 수행하면서 근소하게 이득을 보고
있었고, 계획서 §5("성능 수치를 유리하게 만들기 위해 확정된 C perf, 측정 흐름을 바꾸지
않는다")에 따라 제거하는 것이 옳다.

**효과 확인**: 제거 후에도 report의 `## Auto-HWM Detail` 블록이 첫 공식 측정과
**바이트 단위로 동일**하다(`diff` 확인, 모든 크기에서 publisher `SNDHWM=RCVHWM=1048576`,
subscriber `2097152`). 즉 실효 HWM 조건은 바뀌지 않았고, 이 변경은 §8 재측정의 대형 크기
셀 변동을 설명하지 못한다.

### 7.2 P2 — 구현 내용

`bindings/cpp/src/Runtime/Sockets/pubsub.cpp`에 있던 두 종류의 중복을 제거했다.
계획서 §5의 "성능 개선이 없거나 작더라도 … 중복 제거 … 를 명확히 개선하면 최종 코드로
채택할 수 있다" 항목에 해당한다.

1. `pub_socket_t::publish()`와 `xpub_socket_t::publish()`의 **7줄 본문이 문자 단위로
   동일**했다. publish operation 조립 절차를 파일 지역 `make_publish_state()` 하나가
   소유하게 하고, 두 멤버는 소켓 handle과 callback state만 넘긴다. PUB과 XPUB은 socket
   type만 다르지 publish operation을 만드는 방법이 다르지 않다는 사실이 코드에 한 번만
   적힌다.
2. `sub_socket_t`/`xsub_socket_t`의 `set_subscription`/`unset_subscription`/
   `subscription_at` **6개 함수가 같은 `throw config_error_t (config_result_from_errno
   (zlink_errno ()), zlink_errno ())` 문장을 반복**했다. 파일 지역
   `throw_last_config_error()` 하나로 모았다.

공개 헤더는 건드리지 않았다. 공개 signature, 공개 type, ownership, error 동작(던지는
예외 타입·`internal_errno`·발생 시점)이 모두 그대로다. hot path 호출 시퀀스도 동일하다
(`make_publish_state()`는 같은 TU의 static 함수로 인라인된다).

> 구현 시 주의: `send_operation_t(std::unique_ptr<operation_state_t>)` 생성자는
> protected라 익명 namespace 자유 함수에서 호출할 수 없다. 그래서 helper는
> `send_operation_t`가 아니라 **채워진 state를 반환**하고, `send_operation_t` 생성은
> 각 멤버가 수행한다.

### 7.3 계약 검증 (before/after)

| 검증 | before | after |
|------|--------|-------|
| contract 테스트 (`bindings/cpp/build`, `test_cpp_contract_*` 14개) | 12 통과 / 2 실패 | **12 통과 / 2 실패 (동일)** |
| 실패 2건의 출력 | `test_cpp_contract_request_reply`, `test_cpp_contract_socket` | **`diff` 결과 완전히 동일** — 기존 실패(`log/2026-08-23-c6-fix-and-64k-diagnosis.md` §4.2와 같은 2건) |
| sample smoke (ctest `-L sample-smoke`, 7개) | 6 통과 / 1 실패(`dealer_router_recv_sample`, 기존) | **6 통과 / 1 실패 (동일)** |
| `sample_cpp_pubsub_recv_sample` | 통과 | 통과 |
| 빌드 경고 | 0 | **0** |

---

## 8. 재측정 — 공식 조건 `PUBSUB`/`tcp` 전체 크기

### 8.1 실행한 명령 (C 먼저, 같은 session tag)

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 --pattern PUBSUB --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-official-pubsub-pass2-20260823

PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --core-version 0.12.0 --pattern PUBSUB --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-official-pubsub-pass2-20260823
```

| 대상 | 결과 파일 | status | result lines |
|------|-----------|--------|--------------|
| C | `perf_c_single_linux_20260823_144931_bindings-0.12.0-official-pubsub-pass2-20260823.txt` | **complete** | 30/30 |
| C++ | `perf_cpp_single_linux_20260823_145128_bindings-0.12.0-official-pubsub-pass2-20260823.txt` | **complete** | 30/30 |

- Effective Options: `lang`을 제외한 **모든 항목이 `diff`로 완전히 일치**.
- auto-HWM: C++ report의 `## Auto-HWM Detail`이 첫 공식 측정과 **완전히 동일**
  (`diff` 확인). `MsgUnit(B)`는 두 회차 모두 `?`.
- client 수: `PUBSUB`는 publisher 1 / subscriber 1, memory guard cap 없음.
- Core runtime: 두 report 모두 release `0.12.0`(`META,core_source,release`,
  `META,commit,f99703c219`).

### 8.2 원시 반복값

C (Kmsg/s, latency mean ms):

| Size | run1 | run2 | run3 | median |
|-----:|-----:|-----:|-----:|-------:|
| 64 | 1504.54 / 35.713 | 1504.23 / 28.793 | 1499.32 / 32.397 | **1504.23 / 32.397** |
| 256 | 1201.13 / 28.181 | 1144.89 / 28.924 | 1208.99 / 28.001 | **1201.13 / 28.181** |
| 1024 | 1144.73 / 8.329 | 1105.41 / 8.642 | 1082.57 / 8.725 | **1105.41 / 8.642** |
| 65536 | 52.39 / 2.937 | 52.02 / 2.949 | 48.23 / 3.170 | **52.02 / 2.949** |
| 131072 | 26.39 / 2.937 | 25.43 / 3.041 | 24.22 / 3.185 | **25.43 / 3.041** |
| 262144 | 14.95 / 2.624 | 15.38 / 2.558 | 14.63 / 2.676 | **14.95 / 2.624** |

C++ (Kmsg/s, latency mean ms):

| Size | run1 | run2 | run3 | median |
|-----:|-----:|-----:|-----:|-------:|
| 64 | 1390.79 / 31.599 | 1379.62 / 34.911 | 1376.27 / 32.820 | **1379.62 / 32.820** |
| 256 | 1112.31 / 23.018 | 1107.11 / 30.491 | 1086.02 / 23.215 | **1107.11 / 23.215** |
| 1024 | 993.34 / 9.610 | 1020.79 / 9.307 | 992.05 / 9.631 | **993.34 / 9.610** |
| 65536 | 36.51 / 4.235 | 35.37 / 4.379 | 43.39 / 3.553 | **36.51 / 4.235** |
| 131072 | 20.63 / 3.813 | 21.46 / 3.648 | 22.75 / 3.436 | **21.46 / 3.648** |
| 262144 | 12.90 / 3.052 | 14.55 / 2.701 | 13.25 / 2.974 | **13.25 / 2.974** |

### 8.3 비율과 첫 회 대비

| Size | C median (msg/s) | C++ median (msg/s) | **ratio (pass2)** | ratio (첫 회) | latency ratio (pass2) | latency ratio (첫 회) |
|-----:|-----------------:|-------------------:|------------------:|--------------:|----------------------:|----------------------:|
| 64 | 1,504,228.4 | 1,379,618.0 | **91.72%** | 87.94% | 1.013배 | — |
| 256 | 1,201,133.2 | 1,107,114.0 | **92.17%** | 93.75% | 0.824배 | — |
| 1024 | 1,105,406.4 | 993,339.4 | **89.86%** | 93.19% | 1.112배 | — |
| 65536 | 52,016.6 | 36,507.0 | **70.18%** | 78.29% | 1.436배 | — |
| 131072 | 25,430.8 | 21,455.8 | **84.37%** | 90.04% | 1.200배 | — |
| 262144 | 14,952.0 | 13,247.2 | **88.60%** | 93.16% | 1.133배 | — |
| **aggregate mean** | | | **86.15%** | 89.40% | **1.120배** | 1.062배 |

기록용(판정 입력 아님): 65536B 제외 5셀 aggregate = **89.34%**.

### 8.4 회차 간 변동의 해석

pass2의 aggregate는 첫 회보다 3.25%p **낮다**. 이것은 이번 변경의 효과가 아니다.

- **구현한 변경은 대형 크기 셀에 영향을 줄 수 없다.** P2는 publish 조립 코드의 중복
  제거(호출 시퀀스 불변)이고, P1은 설정 단계의 context 호출 제거인데 §7.1에서 확인했듯이
  **실효 auto-HWM은 두 회차가 완전히 동일**하다.
- **64B는 개선 방향이다**: 87.94% → **91.72%**. 이 셀이 이번 pass가 실제로 건드린
  유일한 셀 계열이다.
- **대형 크기 3셀이 함께 내려간 것은 §3에서 정량화한 allocator 체제 변동이다.** 같은
  세션 안에서도 C++ 65536B가 35.37~43.39 Kmsg/s(±10%), C가 48.23~52.39 Kmsg/s로 흔들리고,
  §3.2에서 보듯 trim 체제 하나로 30~50% 차이가 난다. `PAIR`에서도 같은 셀이 공식 70.90%와
  재확인 88.22%로 갈렸다.
- 두 회차 모두 **미달**(89.40%, 86.15% 모두 목표 95%·완화 90% 미만)이므로 판정 결과는
  달라지지 않는다.

계획서 §8("측정값이 있으면 aggregate 평균으로 판정한다", "변동 폭을 이유로 판정을 미루지
않는다")에 따라 **가장 최근의 complete paired 측정인 pass2를 §9.1.1의 값으로 기록**하고,
첫 회 값은 이 로그와 진행 시트에 이력으로 남긴다.

## 9. 판정

| 항목 | 값 | 기준 | 결과 |
|------|-----|------|------|
| throughput aggregate mean | **86.15%** | 목표 95% / 완화 90% | **미달** |
| 개별 size 최소 기준 | 65536B 70.18%, 131072B 84.37% | 85% | 2개 셀 미달 (65536B는 환경 지배로 판정, §4) |
| latency aggregate mean | **1.120배** | 상한 2.0배 | **통과** |
| Effective Options / auto-HWM / client 수 | 일치 | — | 통과 |

**`PUBSUB`/`tcp` 자체 개선 pass 결과: 종합 미달(86.15%).**
contract를 보존하는 성능 후보가 남아 있지 않다(§7의 P3~P7 모두 no-go). 계획서 §7.4에
따라 다음 단계는 **Sol 리뷰 기반 2차 개선 pass**다. Sol 리뷰까지 마치고도 후보가 없으면
계획서 §8의 `보류` 요건이 충족된다.

Sol 리뷰에 올릴 쟁점은 다음 세 가지다.

1. **P3(builder 계층)** — 64B gap의 79.9%, publish 호출당 약 20 ns가 공개 fluent builder
   계약 자체에 있다. 공개 interface를 유지한 채 이 비용을 줄일 방법이 있는지가 이 pattern의
   유일한 남은 성능 질문이다. (참고: C3에서 스택 state 전환은 이미 no-go로 확정)
2. **65536B 셀의 기록 방식** — `PAIR`과 같은 환경 지배 셀로 확정할지, 다른 처리를 할지.
3. **회차 간 변동(89.40% ↔ 86.15%)** — `최종·경계 판정`(§7.2, 5회) 조건으로 한 번 더
   측정해 기록을 굳힐지.

## 10. 코드·commit 상태

### 10.1 저장소에 남긴 변경

| 파일 | 종류 |
|------|------|
| `bindings/cpp/src/Runtime/Sockets/pubsub.cpp` | **shipping code — P2**(중복 제거, 성능 중립, 공개 API 불변) |
| `bindings/cpp/perf/single/src/perf_pubsub.cpp` | **perf 하네스 — P1**(측정 의미 정렬) |
| `doc/perf/perf/bindings-0.12.0/log/2026-08-23-cpp-pubsub-improvement-pass.md` | 이 문서 |
| `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md` §9.1.1 | `tcp`/`PUBSUB` 행 갱신 |
| `doc/perf/perf/bindings-0.12.0/progress.ko.md` | §3·§4·§5 갱신 |
| `bindings/c/perf/results/single/report/perf_c_single_linux_20260823_144931_*.txt` | 재측정 결과물 |
| `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260823_145128_*.txt` | 재측정 결과물 |

**commit·push 하지 않았다.**

### 10.2 삭제한 throwaway 산출물

모두 scratchpad(`/tmp/claude-1000/...`) 아래에만 만들었고 작업 종료 시점에 삭제했다.

- ablation 변형 소스 `perf_pubsub_variant.cpp`, 실제 트리를 가리키던 symlink
  (`perf/common`, `perf/single/common`), 바이너리 6개
  (`pubsub_base`/`cpub`/`csub`/`cboth`/`mid`/`norecalc`)
- 마이크로 계측 소스 `micro_publish.cpp`와 바이너리

트리에 계측 코드는 남아 있지 않다. `git status --short`로 위 표 외의 변경이 없음을 확인했다.
