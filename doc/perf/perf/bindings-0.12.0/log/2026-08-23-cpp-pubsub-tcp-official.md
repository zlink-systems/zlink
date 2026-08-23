# C++ PUBSUB/tcp 공식(release core) paired 측정 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> §7.1(smoke), §7.2(반복 횟수), §7.3(paired C 규칙), §9.1.1(Single suite 표)
>
> 대상: single suite, `PUBSUB` pattern, `tcp` transport, 재릴리스된
> `core/v0.12.0` release runtime(provenance revision `f99703c219`). C `single`
> `STANDARD_PATTERNS`(`PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,...`)에서 `PAIR`
> 다음 pattern이다. `PAIR`/`tcp`는 이미 자체 pass·Sol 리뷰 pass를 마치고
> `보류(84.92%)`로 확정됐다(계획서 §9.1.1). C++ 바인딩 working tree는 채택
> 후보 C1/C3/C4/C5/C6(진행 시트 §5)를 포함한 현재 상태 그대로 측정했다.
> commit·push는 하지 않았고 코드도 수정하지 않았다.

## 1. 환경 manifest

| 항목 | 값 |
|------|-----|
| host | `ulalax-gram` |
| OS/kernel | `Linux 6.6.87.2-microsoft-standard-WSL2` (WSL2) |
| CPU | `x86_64`, 16 logical cores (`12th Gen Intel(R) Core(TM) i7-1260P`) |
| CPU governor | 읽을 수 없음(`/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` 없음, WSL2에는 cpufreq 미노출) |
| memory | 11Gi total, 측정 시작 시 10Gi free |
| 작업 브랜치 / commit | `codex/bindings-0.12.0-performance` / `f99703c2190b0f6c670be49f67315d904886c742`(HEAD) |
| working tree | C++ candidate C1/C3/C4/C5/C6 구현이 반영된 미커밋 상태(`git status --short`로 확인, commit 안 함) — 이전 `PAIR`/`tcp` 공식 측정 시점과 동일한 후보 집합 |
| Core runtime 소스 | release (`--core-version 0.12.0`) |
| Core runtime prefix | `/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64` |
| Core runtime 파일 | `lib/libzlink.so.0.12.0` |
| Core runtime sha256 | `5c97949fb300daa33231dbe87c39f933d7e37c5e30e7c1a39ca8d993722b8049` (`core-package-provenance.json`의 `runtime.sha256`과 직접 재계산한 sha256sum이 일치) |
| Core release tag | `core/v0.12.0` |
| Core provenance source revision | `f99703c2190b0f6c670be49f67315d904886c742` (현재 HEAD와 일치 — 재릴리스본), `dirty: false` |
| session tag | `bindings-0.12.0-official-pubsub-20260823` |
| 시각(시작) | 2026-08-23 14:13 KST |

release runtime 검증: 두 러너 모두 `--core-version 0.12.0`을 전달했다. C
report의 META 블록(`META,core_source,release` /
`META,core_runtime,/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64/lib/libzlink.so.0.12.0`
/ `META,core_revision,f99703c2190b0f6c670be49f67315d904886c742` /
`META,core_dirty,0`)과 C++ 러너의 별도 검증 재실행 콘솔 출력(`Perf Core
release prefix: /home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64` / `Perf
runtime libzlink: .../linux-x64/lib/libzlink.so.0.12.0`,
`--results-tag bindings-0.12.0-official-pubsub-20260823-verify`)로 두 러너가
같은 release prefix를 사용했음을 확인했다. C++ 공식 report 파일 자체에는
META 블록이 없다(C++ 러너의 기존 동작, `PAIR` 측정 때와 동일) — 콘솔 로그로
대체 검증했다.

## 2. 실행한 명령

### 2.1 공식 smoke (§7.1)

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 \
  --pattern PUBSUB --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-official-pubsub-20260823

PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --core-version 0.12.0 \
  --pattern PUBSUB --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-official-pubsub-20260823
```

결과: 둘 다 `status: complete`.

| 대상 | 결과 파일 | status | throughput | latency mean |
|------|-----------|--------|-----------:|--------------:|
| C | `perf_c_single_linux_20260823_141530_bindings-0.12.0-official-pubsub-20260823.txt` | complete | 1,429,786.000 msg/s | 22.165 ms |
| C++ | `perf_cpp_single_linux_20260823_141537_bindings-0.12.0-official-pubsub-20260823.txt` | complete | 1,392,531.000 msg/s | 42.964 ms |

**smoke 판정: 통과.** 두 report 모두 release runtime, `status: complete`.

### 2.2 전체 크기 공식 측정 (§7.2 후보 판정 단계, `--runs 3`)

크기: 계획서 §3.1 Single suite 전체 — 64, 256, 1024, 65536, 131072, 262144
bytes. duration 기본값(5초), runs 3.

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 \
  --pattern PUBSUB --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-official-pubsub-20260823

PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --core-version 0.12.0 \
  --pattern PUBSUB --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-official-pubsub-20260823
```

C를 먼저 완주(`status: complete`)시킨 뒤 바로 이어서 C++를 같은 조건·같은
session tag로 실행했다. 두 프로세스는 순차 실행했고(§7.0), 다른 perf
프로세스를 동시에 실행하지 않았다(조용한 host, 위 환경 manifest 기준).

| 대상 | 결과 파일 | status | expected/actual result lines |
|------|-----------|--------|-------------------------------|
| C | `perf_c_single_linux_20260823_141558_bindings-0.12.0-official-pubsub-20260823.txt` | **complete** | 30/30 |
| C++ | `perf_cpp_single_linux_20260823_141752_bindings-0.12.0-official-pubsub-20260823.txt` | **complete** | 30/30 |

Effective Options 일치: `lang` 제외 모든 항목(`runs=3`,
`duration_seconds=5`, `timeout_seconds=45`, `io_threads=1`, `hwm=auto-hwm`,
`sndhwm/rcvhwm=auto-hwm`, `sndbuf/rcvbuf=-1`, `sndtimeo_ms/rcvtimeo_ms=200`,
`ctx_auto_hwm_enable=core-default`, `ctx_auto_hwm_profile=balanced`,
`patterns=PUBSUB`, `transports=tcp`, `msg_sizes` 동일)가 두 report에서
`diff`로 확인한 결과 완전히 일치한다.

auto-HWM: C++ report의 Auto-HWM Detail 표는 모든 크기에서 publisher
`SNDHWM=RCVHWM=1048576`, subscriber `SNDHWM=RCVHWM=2097152`로 동일하다.
`MsgUnit(B)`는 두 socket 모두 `?`로 기록돼 해석 불가 — C report에는 이 절
자체가 없어(C 러너가 Auto-HWM Detail을 출력하지 않음) 직접 비교는 못 했지만,
조건 정렬에 필요한 옵션은 Effective Options로 이미 확인했으므로 이 항목이
paired 결과의 유효성을 바꾸지 않는다.

client 수: `PUBSUB`는 publisher 1 / subscriber 1의 단순 one-way 소켓 쌍이며
memory guard/STREAM client 개념이 적용되지 않는다(계획서 §4의 client 수
확인은 multi 전용). cap 발생 없음.

## 3. 크기별 원시 반복값 (median 대표값의 근거)

### C (release core, `PUBSUB`/`tcp`)

| Size | run | throughput (msg/s) | latency mean (ms) |
|-----:|----:|--------------------:|-------------------:|
| 64B | 1 | 1,557,600 | 33.559 |
| 64B | 2 | 1,490,950 | 34.019 |
| 64B | 3 | 1,497,340 | 32.272 |
| 256B | 1 | 1,172,830 | 27.516 |
| 256B | 2 | 1,196,290 | 28.984 |
| 256B | 3 | 1,102,270 | 26.252 |
| 1024B | 1 | 1,090,500 | 8.728 |
| 1024B | 2 | 1,081,900 | 8.816 |
| 1024B | 3 | 1,049,260 | 9.081 |
| 65536B | 1 | 46,110 | 3.309 |
| 65536B | 2 | 49,610 | 3.095 |
| 65536B | 3 | 49,040 | 3.104 |
| 131072B | 1 | 24,910 | 3.102 |
| 131072B | 2 | 24,380 | 3.173 |
| 131072B | 3 | 24,880 | 3.104 |
| 262144B | 1 | 14,830 | 2.579 |
| 262144B | 2 | 14,900 | 2.645 |
| 262144B | 3 | 15,340 | 2.568 |

### C++ (release core, candidate C1/C3/C4/C5/C6 포함, `PUBSUB`/`tcp`)

| Size | run | throughput (msg/s) | latency mean (ms) |
|-----:|----:|--------------------:|-------------------:|
| 64B | 1 | 1,316,800 | 27.290 |
| 64B | 2 | 1,281,040 | 30.055 |
| 64B | 3 | 1,363,860 | 35.517 |
| 256B | 1 | 1,101,010 | 29.044 |
| 256B | 2 | 1,099,570 | 24.117 |
| 256B | 3 | 1,092,970 | 22.089 |
| 1024B | 1 | 1,021,050 | 9.168 |
| 1024B | 2 | 992,860 | 9.620 |
| 1024B | 3 | 1,008,270 | 9.443 |
| 65536B | 1 | 38,500 | 4.006 |
| 65536B | 2 | 37,040 | 4.197 |
| 65536B | 3 | 38,400 | 4.028 |
| 131072B | 1 | 23,210 | 3.379 |
| 131072B | 2 | 22,400 | 3.501 |
| 131072B | 3 | 20,600 | 3.824 |
| 262144B | 1 | 13,800 | 2.861 |
| 262144B | 2 | 14,030 | 2.814 |
| 262144B | 3 | 13,880 | 2.840 |

## 4. Median (대표값) 요약과 C++/C 비율

median throughput/latency는 러너가 `RESULT,...` 라인으로 직접 산출한 값을
사용했다(3회 반복의 median, §7.2 규칙).

| Size | C median throughput | C++ median throughput | throughput 비율(C++/C) | C median latency | C++ median latency | latency 비율(C++/C) |
|-----:|---------------------:|------------------------:|--------------------------:|-------------------:|----------------------:|------------------------:|
| 64B | 1,497,337.400 | 1,316,802.000 | **87.94%** | 33.559 ms | 30.055 ms | **0.896배** |
| 256B | 1,172,834.800 | 1,099,570.400 | **93.75%** | 27.516 ms | 24.117 ms | **0.876배** |
| 1024B | 1,081,898.600 | 1,008,271.000 | **93.19%** | 8.816 ms | 9.443 ms | **1.071배** |
| 65536B | 49,043.000 | 38,396.000 | **78.29%** | 3.104 ms | 4.028 ms | **1.298배** |
| 131072B | 24,883.200 | 22,403.600 | **90.04%** | 3.104 ms | 3.501 ms | **1.128배** |
| 262144B | 14,896.600 | 13,877.600 | **93.16%** | 2.579 ms | 2.840 ms | **1.101배** |

- throughput ratio 산술평균(aggregate mean) = **89.40%**
- 평균 latency ratio 산술평균(aggregate mean) = **1.062배**

## 5. 판정 (계획서 §2.1/§2.2, §8 규칙 적용)

`PUBSUB`는 계획서 §2.1의 pattern 그룹 표에서 "단순 one-way" 그룹에 속한다
(`PAIR`, `PUBSUB`, `DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_STREAM`).

- C++ 단순 one-way 목표: **최소 기준 85% / 중앙값(aggregate mean) 목표 95%**
  (완화 목표 선택 시 90%).
- C++/Rust latency 상한: **평균 latency의 C 대비 최대 2.0배**. 계획서
  §2.2에는 C++에 대한 `PUBSUB` 전용 latency 상한 예외가 없다(예외 표는
  `.NET`의 특정 transport·size 셀만 다룬다) — 표준 2.0배 상한을 그대로
  적용했다.

**Throughput**: aggregate mean **89.40%**는 기본 목표 95%에 미달하고, 완화
목표 90%에도 근소하게(0.60%p) 못 미친다. 개별 셀 중 65536B(78.29%)는 개별
최소 기준 85%에도 미달하는 outlier다. 64B(87.94%)는 개별 최소 85%는 넘지만
낮은 편이다. 256B/1024B/131072B/262144B는 90.04~93.75%로 개별 최소 85%는
넉넉히 넘지만 중앙값 목표 95%에는 못 미친다.

**Latency**: aggregate mean **1.062배**는 상한 2.0배 이내로 통과. 개별 셀
outlier 없음(모두 0.876~1.298배 범위, C보다 빠른 셀도 2개 있음 — 64B/256B는
C++가 C보다 latency가 낮다).

**종합 판정: 미달(throughput aggregate mean 89.40%)** — throughput이 §8의
통과 조건(aggregate mean이 pattern 그룹 목표 충족)을 만족하지 못했으므로
latency가 상한 이내라도 전체 상태는 `통과`가 아니다.

`보류`로 기록하지 않는 이유(§8 규칙): `보류`는 "paired 측정, 자체 개선
pass, Sol 리뷰 기반 두 번째 개선 pass를 완료했지만 public contract를 유지한
추가 개선 요소가 없어 현재 aggregate 목표를 달성하지 못한 채 다음 대상으로
이동"하는 경우에만 쓴다. 이번 측정은 `PUBSUB`/`tcp`에 대한 **첫 공식
paired 측정**이다. working tree에 반영된 C1/C3/C4/C5/C6는 `PAIR`/`tcp`
개선 작업에서 채택된 후보이며 `PUBSUB`에 대해서는 아직 pattern 전용 자체
pass나 Sol 리뷰 pass가 수행되지 않았다(C1/C3/C4/C5 로그의 부수 확인에서
`PUBSUB` +7.80% local 개선이 관측된 바 있으나, 이는 §7.4 순서상의 정식
pattern 전용 pass가 아니다). 따라서 계획서 §7.4 순서(자체 pass → Sol 리뷰
pass → 판정)가 아직 `PUBSUB`에 대해 완료되지 않았으므로 `미달(89.40%)`로
기록하고 `보류`로 바꾸지 않는다.

참고: `PAIR`/`tcp`의 최종 판정은 `보류(84.92%)`였다(로그
`log/2026-08-23-cpp-pair-tcp-official.md`). `PUBSUB`의 aggregate throughput
89.40%는 `PAIR`보다 약 4.48%p 높다 — C1~C5 구현 로그(진행 시트 §5)에 기록된
`PUBSUB` local +7.80% 부수 개선 방향과 일치하며, 두 pattern이 공유하는 hot
path 개선(C1 raw send, C4 recv guard, C6 correctness fix)의 영향으로 보인다.
다만 이 값 자체는 `PUBSUB` 전용 자체 pass 없이 얻어진 것이므로 §7.4 순서를
생략할 근거로는 쓰지 않는다.

## 6. 다음 조치 (계획 §7.4 순서 참고, 이번 작업 범위 밖)

1. `PUBSUB`/`tcp`에 대한 자체 개선 pass(§7.4)를 수행한다 — 65536B outlier
   (78.29%)와 64B(87.94%)를 우선 검토 대상으로 기록한다. `PAIR`에서
   65536B outlier가 glibc heap-trim page fault storm(환경 요인, binding
   결함 아님)으로 판명된 진단(`log/2026-08-23-c6-fix-and-64k-diagnosis.md`)이
   `PUBSUB`에도 적용되는지 우선 확인한다.
2. 자체 pass 뒤에도 미달이면 Sol에 read-only review를 요청하고, 계약을
   보존하는 후보가 있으면 두 번째 개선 pass를 수행해 after를 다시 공식
   release runtime으로 측정한다.
3. 두 pass를 모두 마쳤는데도 계약 보존 후보가 없으면 §8 규칙에 따라
   `보류`로 pattern 결과를 확정한다.
4. `tcp`의 `PUBSUB`가 확정되면(통과/보류) 계획서 §7.4 순서에 따라 C
   `STANDARD_PATTERNS`의 다음 pattern(`DEALER_DEALER`)으로 진행한다.

## 7. 코드·commit 상태

이번 세션에서 코드는 수정하지 않았다(측정만 수행). 기존에 working tree에
남아 있던 C++ candidate C1/C3/C4/C5/C6 변경(§git status)도 그대로 두었고,
commit·push는 수행하지 않았다.
