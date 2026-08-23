# C++ PAIR/tcp 공식(release core) paired 측정 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> §7.1(smoke), §7.2(반복 횟수), §7.3(paired C 규칙), §9.1.1(Single suite 표)
>
> 대상: single suite, `PAIR` pattern, `tcp` transport, 재릴리스된
> `core/v0.12.0` release runtime(provenance revision `f99703c219`). C++ 바인딩
> working tree는 채택 후보 C1/C3/C4/C5(진행 시트 §5)를 포함한 현재 상태 그대로
> 측정했다. commit·push는 하지 않았다.

## 1. 환경 manifest

| 항목 | 값 |
|------|-----|
| host | `ulalax-gram` |
| OS/kernel | `Linux 6.6.87.2-microsoft-standard-WSL2` (WSL2) |
| CPU | `x86_64`, 16 logical cores (`12th Gen Intel(R) Core(TM) i7-1260P`) |
| CPU governor | 읽을 수 없음(`/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` 없음, WSL2에는 cpufreq 미노출) |
| memory | 11Gi total, 측정 시작 시 9.9Gi free |
| 작업 브랜치 / commit | `codex/bindings-0.12.0-performance` / `f99703c2190b0f6c670be49f67315d904886c742`(HEAD) |
| working tree | C++ candidate C1/C3/C4/C5 구현이 반영된 미커밋 상태(`git status --short`로 확인, commit 안 함) |
| Core runtime 소스 | release (`--core-version 0.12.0`) |
| Core runtime prefix | `/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64` |
| Core runtime 파일 | `lib/libzlink.so.0.12.0` |
| Core runtime sha256 | `5c97949fb300daa33231dbe87c39f933d7e37c5e30e7c1a39ca8d993722b8049` (`core-package-provenance.json`의 `runtime.sha256`과 직접 재계산한 sha256sum이 일치) |
| Core release tag | `core/v0.12.0` |
| Core provenance source revision | `f99703c2190b0f6c670be49f67315d904886c742` (현재 HEAD와 일치 — 재릴리스본) |
| Core provenance dirty | `false` |
| session tag | `bindings-0.12.0-official-20260823` |
| 시각(시작) | 2026-08-23 13:23 KST |

release runtime 검증: 두 러너 모두 `--core-version 0.12.0`을 전달했다. C
report의 META 블록(`META,core_source,release` /
`META,core_runtime,/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64/lib/libzlink.so.0.12.0`
/ `META,core_revision,f99703c2190b0f6c670be49f67315d904886c742`)과 C++ 러너
콘솔 출력(`Perf Core release prefix:
/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64` /
`Perf runtime libzlink: .../linux-x64/lib/libzlink.so.0.12.0`, 검증용 재실행
`..._132402_..-verify.txt`)로 두 러너가 같은 release prefix를 사용했음을
확인했다. C++ 공식 report 파일 자체에는 META 블록이 없다(C++ 러너의 기존
동작이며 이번 측정에서 수정하지 않음) — 콘솔 로그로 대체 검증했다.

## 2. 실행한 명령

### 2.1 공식 smoke (§7.1)

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-official-20260823

PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --core-version 0.12.0 \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-official-20260823
```

결과: 둘 다 `status: complete`.

| 대상 | 결과 파일 | status | throughput | latency mean |
|------|-----------|--------|-----------:|--------------:|
| C | `perf_c_single_linux_20260823_132304_bindings-0.12.0-official-20260823.txt` | complete | 2,528,435.000 msg/s | 52.353 ms |
| C++ | `perf_cpp_single_linux_20260823_132311_bindings-0.12.0-official-20260823.txt` | complete | 2,081,149.000 msg/s | 61.587 ms |

**smoke 판정: 통과.** 두 report 모두 release runtime, `status: complete`.
지난 세션(`log/2026-08-23-cpp-single-smoke.md`)에서 관측된 TCP
connection-ready deadlock(commit 이전의 결함 release asset)이 재릴리스된
release asset에서는 재현되지 않는다.

### 2.2 전체 크기 공식 측정 (§7.2 후보 판정 단계, `--runs 3`)

크기: 계획서 §3.1 Single suite 전체 — 64, 256, 1024, 65536, 131072, 262144
bytes. duration 기본값(5초), runs 3.

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 \
  --pattern PAIR --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-official-20260823

PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --core-version 0.12.0 \
  --pattern PAIR --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-official-20260823
```

C를 먼저 완주(`status: complete`)시킨 뒤 바로 이어서 C++를 같은 조건·같은
session tag로 실행했다. 두 프로세스는 순차 실행했고(§7.0), 다른 perf
프로세스를 동시에 실행하지 않았다.

| 대상 | 결과 파일 | status | expected/actual result lines |
|------|-----------|--------|-------------------------------|
| C | `perf_c_single_linux_20260823_132408_bindings-0.12.0-official-20260823.txt` | **complete** | 30/30 |
| C++ | `perf_cpp_single_linux_20260823_132544_bindings-0.12.0-official-20260823.txt` | **complete** | 30/30 |

Effective Options 일치: `lang` 제외 모든 항목(`runs=3`,
`duration_seconds=5`, `timeout_seconds=45`, `io_threads=1`, `hwm=auto-hwm`,
`sndhwm/rcvhwm=auto-hwm`, `sndbuf/rcvbuf=-1`, `sndtimeo_ms/rcvtimeo_ms=200`,
`ctx_auto_hwm_enable=core-default`, `ctx_auto_hwm_profile=balanced`,
`patterns=PAIR`, `transports=tcp`, `msg_sizes` 동일)가 두 report에서 완전히
일치한다.

auto-HWM: C++ report의 Auto-HWM Detail 표는 모든 크기에서
`SNDHWM=RCVHWM=1048576`으로 동일하다. `MsgUnit(B)`는 두 socket 모두 `?`로
기록돼 해석 불가 — C report에는 이 절 자체가 없어(C 러너가 Auto-HWM Detail을
출력하지 않음) 직접 비교는 못 했지만, 조건 정렬에 필요한 옵션은 Effective
Options로 이미 확인했으므로 이 항목이 paired 결과의 유효성을 바꾸지 않는다.

client 수: `PAIR`는 1:1 소켓 쌍이며 memory guard/STREAM client 개념이
적용되지 않는다(계획서 §4의 client 수 확인은 multi 전용). cap 발생 없음.

## 3. 크기별 원시 반복값 (median 대표값의 근거)

### C (release core, `PAIR`/`tcp`)

| Size | run | throughput (msg/s) | latency mean (ms) |
|-----:|----:|--------------------:|-------------------:|
| 64B | 1 | 2,706,780 | 51.294 |
| 64B | 2 | 2,605,110 | 49.159 |
| 64B | 3 | 2,653,330 | 52.286 |
| 256B | 1 | 1,973,140 | 19.552 |
| 256B | 2 | 1,952,350 | 19.708 |
| 256B | 3 | 1,982,350 | 19.379 |
| 1024B | 1 | 1,192,320 | 8.284 |
| 1024B | 2 | 1,181,680 | 8.314 |
| 1024B | 3 | 1,202,830 | 7.289 |
| 65536B | 1 | 46,570 | 3.303 |
| 65536B | 2 | 47,300 | 3.264 |
| 65536B | 3 | 46,450 | 3.323 |
| 131072B | 1 | 27,320 | 2.844 |
| 131072B | 2 | 27,860 | 2.786 |
| 131072B | 3 | 27,410 | 2.829 |
| 262144B | 1 | 16,220 | 2.410 |
| 262144B | 2 | 16,110 | 2.439 |
| 262144B | 3 | 16,180 | 2.423 |

### C++ (release core, candidate C1/C3/C4/C5 포함, `PAIR`/`tcp`)

| Size | run | throughput (msg/s) | latency mean (ms) |
|-----:|----:|--------------------:|-------------------:|
| 64B | 1 | 2,233,780 | 50.822 |
| 64B | 2 | 2,094,650 | 65.585 |
| 64B | 3 | 2,147,210 | 64.171 |
| 256B | 1 | 1,845,300 | 20.869 |
| 256B | 2 | 1,898,340 | 20.170 |
| 256B | 3 | 1,775,020 | 21.578 |
| 1024B | 1 | 1,100,390 | 8.944 |
| 1024B | 2 | 1,106,330 | 8.873 |
| 1024B | 3 | 1,070,390 | 9.228 |
| 65536B | 1 | 32,780 | 4.813 |
| 65536B | 2 | 33,020 | 4.797 |
| 65536B | 3 | 39,590 | 3.983 |
| 131072B | 1 | 23,480 | 3.371 |
| 131072B | 2 | 22,110 | 3.583 |
| 131072B | 3 | 22,810 | 3.469 |
| 262144B | 1 | 14,970 | 2.618 |
| 262144B | 2 | 14,340 | 2.732 |
| 262144B | 3 | 13,930 | 2.814 |

## 4. Median (대표값) 요약과 C++/C 비율

median throughput/latency는 러너가 `RESULT,...` 라인으로 직접 산출한 값을
사용했다(3회 반복의 median, §7.2 규칙).

| Size | C median throughput | C++ median throughput | throughput 비율(C++/C) | C median latency | C++ median latency | latency 비율(C++/C) |
|-----:|---------------------:|------------------------:|--------------------------:|-------------------:|----------------------:|------------------------:|
| 64B | 2,653,326.200 | 2,147,210.000 | **80.93%** | 51.294 ms | 64.171 ms | **1.251배** |
| 256B | 1,973,139.800 | 1,845,298.600 | **93.52%** | 19.552 ms | 20.869 ms | **1.067배** |
| 1024B | 1,192,324.800 | 1,100,387.600 | **92.29%** | 8.284 ms | 8.944 ms | **1.080배** |
| 65536B | 46,574.000 | 33,022.600 | **70.90%** | 3.303 ms | 4.797 ms | **1.452배** |
| 131072B | 27,406.800 | 22,806.800 | **83.22%** | 2.829 ms | 3.469 ms | **1.226배** |
| 262144B | 16,180.200 | 14,342.200 | **88.64%** | 2.423 ms | 2.732 ms | **1.128배** |

- throughput ratio 산술평균(aggregate mean) = **84.92%**
- 평균 latency ratio 산술평균(aggregate mean) = **1.201배**

## 5. 판정 (계획서 §2.1/§2.2, §8 규칙 적용)

`PAIR`는 계획서 §2.1의 pattern 그룹 표에서 "단순 one-way" 그룹에 속한다
(`PAIR`, `PUBSUB`, `DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_STREAM`).

- C++ 단순 one-way 목표: **최소 기준 85% / 중앙값(aggregate mean) 목표 95%**
  (완화 목표 선택 시 90%, 완화도 별도로 기록해야 함).
- C++/Rust latency 상한: **평균 latency의 C 대비 최대 2.0배**.

**Throughput**: aggregate mean **84.92%**는 기본 목표 95%뿐 아니라 완화
목표 90%에도 미달한다. 개별 셀 중 64B(80.93%)와 65536B(70.90%)는 개별 최소
기준 85%에도 미달하는 outlier다. 256B/1024B/131072B/262144B는 개별 최소
85%는 넘지만(88.64~93.52%) 중앙값 목표 95%에는 못 미친다.

**Latency**: aggregate mean **1.201배**는 상한 2.0배 이내로 통과. 개별 셀
outlier 없음(모두 1.05~1.46배 범위).

**종합 판정: 미달(throughput aggregate mean 84.92%, latency aggregate mean
1.201배)** — throughput이 §8의 통과 조건(aggregate mean이 pattern 그룹 목표
충족)을 만족하지 못했으므로 latency가 상한 이내라도 전체 상태는 `통과`가
아니다.

이 측정은 이미 C1/C3/C4/C5 후보(진행 시트 §5, "채택 후보 (예비 판정)" /
"구조개선 (예비 판정)")가 반영된 **자체 개선 pass 이후**의 working tree
기준이다. 계획서 §7.4 순서상 다음 단계는 11번(Sol review-based 두 번째
개선 pass)이며, 진행 시트 §3에 "Sol 리뷰 pass: 미착수"로 기록돼 있는 그대로
아직 수행되지 않았다. 계획서 §8의 `보류` 정의("자체 개선 pass, Sol 리뷰
기반 두 번째 개선 pass를 완료했지만... ")는 두 pass가 모두 끝난 뒤에만
적용되므로, 이번 공식 측정 결과는 **`미달(84.92%)`**로 기록하고 `보류`로는
바꾸지 않는다. Sol review pass를 진행하거나, 진행하지 않기로 결정하면 그
근거를 별도로 기록한 뒤 최종 `보류` 또는 추가 개선 판정을 내린다.

이 결과는 성능 목표 미달이지만 작업 실패가 아니다 — C1~C5는 이미 예비
판정을 받은 채택/구조개선 후보이며, 사용자는 구조적 개선을 성능 향상 없이도
수용한다는 전제가 확인돼 있다(진행 시트 §5).

## 6. 다음 조치 (계획 §7.4 순서 참고, 이번 작업 범위 밖)

1. §7.4 11단계: Sol에 read-only review를 요청하고, 계약을 보존하는 후보가
   있으면 두 번째 개선 pass를 수행해 after를 다시 공식 release runtime으로
   측정한다.
2. Sol이 안전한 후보를 제시하지 않으면 no-go 판단을 기록하고 §8 규칙에 따라
   `보류`로 pattern 결과를 확정한다.
3. `tcp`가 확정되면(통과/보류) 계획서 §7.4 15~17단계에 따라 `PAIR`의 다음
   transport(`ws`)로 진행한다.

## 7. 코드·commit 상태

이번 세션에서 코드는 수정하지 않았다(측정만 수행). 기존에 working tree에
남아 있던 C++ candidate C1/C3/C4/C5 변경(§git status)도 그대로 두었고,
commit·push는 수행하지 않았다.
