# C++ DEALER_DEALER/tcp 공식(release core) paired 측정 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> §7.1(smoke), §7.2(반복 횟수), §7.3(paired C 규칙), §9.1.1(Single suite 표)
>
> 대상: single suite, `DEALER_DEALER` pattern, `tcp` transport, 재릴리스된
> `core/v0.12.0` release runtime(provenance revision `f99703c219`). C
> `single` `STANDARD_PATTERNS`(`PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,...`)에서
> `PAIR`/`PUBSUB` 다음 pattern이다. `PAIR`/`tcp`는 `보류(84.92%)`,
> `PUBSUB`/`tcp`는 `보류(87.76%)`로 이미 확정됐다(계획서 §9.1.1). C++ 바인딩
> working tree는 채택 후보 C1/C3/C4/C5/C6(진행 시트 §5)를 포함한 현재 상태
> 그대로 측정했다. commit·push는 하지 않았고 코드도 수정하지 않았다.

## 1. 환경 manifest

| 항목 | 값 |
|------|-----|
| host | `ulalax-gram` |
| OS/kernel | `Linux 6.6.87.2-microsoft-standard-WSL2` (WSL2) |
| CPU | `x86_64`, 16 logical cores (`12th Gen Intel(R) Core(TM) i7-1260P`) |
| CPU governor | 읽을 수 없음(WSL2에는 cpufreq 미노출) |
| memory | 11Gi total, 측정 시작 시 10Gi free, load average 0.02/0.43/0.83(조용한 host) |
| 작업 브랜치 / commit | `codex/bindings-0.12.0-performance` / `8a5a0361da`(HEAD) |
| working tree | 커밋 없음, 수정 없음(`git status --short` 확인 결과 clean) — 이전 `PAIR`/`PUBSUB` 공식 측정과 동일한 candidate 집합(C1/C3/C4/C5/C6가 이미 HEAD에 반영됨) |
| Core runtime 소스 | release (`--core-version 0.12.0`) |
| Core runtime prefix | `/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64` |
| Core runtime 파일 | `lib/libzlink.so.0.12.0` |
| Core release tag | `core/v0.12.0` |
| Core provenance source revision | `f99703c2190b0f6c670be49f67315d904886c742`(HEAD와 일치, `dirty: false`) |
| session tag | `bindings-0.12.0-official-dd-20260823` |
| 시각(시작) | 2026-08-23 15:39 KST |

release runtime 검증: 두 러너 모두 `--core-version 0.12.0`을 전달했다. C
report의 META 블록(`META,core_source,release` /
`META,core_runtime,/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64/lib/libzlink.so.0.12.0`
/ `META,core_revision,f99703c2190b0f6c670be49f67315d904886c742` /
`META,core_dirty,0` / `META,core_release_tag,core/v0.12.0`)로 release
provenance revision(`f99703c219`)과 일치함을 확인했다. C++ 공식 report
파일 자체에는 META 블록이 없다(C++ 러너의 기존 동작, `PAIR`/`PUBSUB` 측정
때와 동일) — 두 러너에 같은 `--core-version 0.12.0` 인자를 전달했고, C
report의 META로 release prefix가 실제로 사용됐음을 확인했으므로 이를
대체 검증으로 사용한다.

## 2. 실행한 명령

### 2.1 공식 smoke (§7.1)

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 \
  --pattern DEALER_DEALER --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-official-dd-20260823

PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --core-version 0.12.0 \
  --pattern DEALER_DEALER --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-official-dd-20260823
```

결과: 둘 다 `status: complete`.

| 대상 | 결과 파일 | status | throughput | latency mean |
|------|-----------|--------|-----------:|--------------:|
| C | `perf_c_single_linux_20260823_153908_bindings-0.12.0-official-dd-20260823.txt` | complete | 2,558,848.000 msg/s | 48.432 ms |
| C++ | `perf_cpp_single_linux_20260823_153914_bindings-0.12.0-official-dd-20260823.txt` | complete | 9,829.000 msg/s | 0.115 ms |

**smoke 판정: 통과(절차상)** — 두 report 모두 release runtime, `status:
complete`. 다만 64B 단일 값에서 이미 C++ throughput이 C의 약 0.38%에
불과하고 latency는 오히려 C보다 훨씬 낮다(0.115 ms vs 48.432 ms) — 전체
크기 측정으로 이어서 확인한다(§3~§5).

### 2.2 전체 크기 공식 측정 (§7.2 후보 판정 단계, `--runs 3`)

크기: 계획서 §3.1 Single suite 전체 — 64, 256, 1024, 65536, 131072, 262144
bytes. duration 기본값(5초), runs 3.

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 \
  --pattern DEALER_DEALER --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-official-dd-20260823

PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --core-version 0.12.0 \
  --pattern DEALER_DEALER --transports tcp \
  --msg-sizes 64,256,1024,65536,131072,262144 --runs 3 \
  --results-tag bindings-0.12.0-official-dd-20260823
```

C를 먼저 완주(`status: complete`)시킨 뒤 바로 이어서 C++를 같은 조건·같은
session tag로 실행했다. 두 프로세스는 순차 실행했고(§7.0), 다른 perf
프로세스를 동시에 실행하지 않았다(조용한 host, 위 환경 manifest 기준).

| 대상 | 결과 파일 | status | expected/actual result lines |
|------|-----------|--------|-------------------------------|
| C | `perf_c_single_linux_20260823_153937_bindings-0.12.0-official-dd-20260823.txt` | **complete** | 30/30 |
| C++ | `perf_cpp_single_linux_20260823_154113_bindings-0.12.0-official-dd-20260823.txt` | **complete** | 30/30 |

Effective Options 일치: `lang` 제외 모든 항목(`runs=3`,
`duration_seconds=5`, `timeout_seconds=45`, `io_threads=1`, `hwm=auto-hwm`,
`sndhwm/rcvhwm=auto-hwm`, `sndbuf/rcvbuf=-1`, `sndtimeo_ms/rcvtimeo_ms=200`,
`ctx_auto_hwm_enable=core-default`, `ctx_auto_hwm_profile=balanced`,
`patterns=DEALER_DEALER`, `transports=tcp`, `msg_sizes` 동일)가 두
report에서 `diff`로 확인한 결과 완전히 일치한다(exit 0).

auto-HWM: C++ report의 Auto-HWM Detail 표는 모든 크기에서 receiver/sender
모두 `SNDHWM=RCVHWM=1048576`으로 동일하다. `MsgUnit(B)`는 두 socket 모두
`?`로 기록돼 해석 불가 — C report에는 이 절 자체가 없어(C 러너가 Auto-HWM
Detail을 출력하지 않음) 직접 비교는 못 했지만, 조건 정렬에 필요한 옵션은
Effective Options로 이미 확인했으므로 이 항목이 paired 결과의 유효성을
바꾸지 않는다.

client 수: `DEALER_DEALER`는 sender 1 / receiver 1의 단순 one-way 소켓
쌍이며 memory guard/STREAM client 개념이 적용되지 않는다(계획서 §4의
client 수 확인은 multi 전용). cap 발생 없음.

## 3. 크기별 median 값 (러너가 직접 산출)

두 러너 모두 콘솔에는 3-run 상세 표와 median 표를 함께 출력하지만, 이
로그를 작성할 때 콘솔 캡처를 앞부분까지 온전히 보존하지 못했다(뒷부분
tail만 확보). 따라서 run별 원시값 대신, report 파일에 실제로 저장되고
판정에 사용하는 `RESULT,...` median 값만 아래에 기록한다(§7.2/§8 규칙상
판정에는 median만 필요하다). C++ report는 §2.2에서 콘솔 median 표까지
확보했으므로 참고용으로 함께 남긴다.

### C (release core, `DEALER_DEALER`/`tcp`) — median (report `RESULT` 라인)

| Size | median throughput (msg/s) | median latency mean (ms) | latency p95 (ms) | latency p99 (ms) |
|-----:|----------------------------:|----------------------------:|------------------:|------------------:|
| 64B | 2,633,372.400 | 51.713 | 55.656 | 57.997 |
| 256B | 2,003,676.200 | 19.111 | 21.244 | 23.256 |
| 1024B | 1,149,967.000 | 8.556 | 9.794 | 10.421 |
| 65536B | 46,352.000 | 3.325 | 4.011 | 4.466 |
| 131072B | 27,449.400 | 2.833 | 3.478 | 3.942 |
| 262144B | 16,205.400 | 2.419 | 2.872 | 3.321 |

### C++ (release core, candidate C1/C3/C4/C5/C6 포함, `DEALER_DEALER`/`tcp`)

| Size | median throughput (msg/s) | median latency mean (ms) | latency p95 (ms) | latency p99 (ms) |
|-----:|----------------------------:|----------------------------:|------------------:|------------------:|
| 64B | 9,849.4 | 0.115 | 0.136 | 0.222 |
| 256B | 9,822.0 | 0.116 | 0.139 | 0.220 |
| 1024B | 9,818.2 | 0.116 | 0.137 | 0.211 |
| 65536B | 10,018.2 | 0.128 | 0.153 | 0.224 |
| 131072B | 9,620.6 | 0.143 | 0.185 | 0.321 |
| 262144B | 9,037.2 | 0.175 | 0.226 | 0.478 |

C++ 결과의 특이점: throughput이 메시지 크기와 거의 무관하게 약
9,000~10,000 msg/s로 평탄하다(C는 메시지 크기가 커질수록 처리량이
자연스럽게 감소한다). latency mean도 모든 크기에서 C보다 훨씬 낮다
(0.115~0.175 ms vs C 2.4~52.4 ms). 이는 두 값이 서로 무관한 우연이
아니라 하나의 원인(§5 참고)을 가리킨다 — 대역폭이 아니라 초당 요청 수
자체가 상한(약 10K msg/s, 메시지당 약 0.1 ms)에 걸려 있고, 그 상한 안에서
는 in-flight 메시지 수가 적어 latency도 낮게 관측된다.

## 4. Median (대표값) 요약과 C++/C 비율

median throughput/latency는 러너가 `RESULT,...` 라인으로 직접 산출한 값을
사용했다(3회 반복의 median, §7.2 규칙).

| Size | C median throughput | C++ median throughput | throughput 비율(C++/C) | C median latency | C++ median latency | latency 비율(C++/C) |
|-----:|---------------------:|------------------------:|--------------------------:|-------------------:|----------------------:|------------------------:|
| 64B | 2,633,372.400 | 9,849.400 | **0.37%** | 51.713 ms | 0.115 ms | **0.0022배** |
| 256B | 2,003,676.200 | 9,822.000 | **0.49%** | 19.111 ms | 0.116 ms | **0.0061배** |
| 1024B | 1,149,967.000 | 9,818.200 | **0.85%** | 8.556 ms | 0.116 ms | **0.0136배** |
| 65536B | 46,352.000 | 10,018.200 | **21.61%** | 3.325 ms | 0.128 ms | **0.0385배** |
| 131072B | 27,449.400 | 9,620.600 | **35.05%** | 2.833 ms | 0.143 ms | **0.0505배** |
| 262144B | 16,205.400 | 9,037.200 | **55.77%** | 2.419 ms | 0.175 ms | **0.0723배** |

- throughput ratio 산술평균(aggregate mean) = **19.02%**
- 평균 latency ratio 산술평균(aggregate mean) = **0.031배**(C보다 훨씬 낮음 — 상한 위반 아님)

## 5. 판정 (계획서 §2.1/§2.2, §8 규칙 적용)

`DEALER_DEALER`는 계획서 §2.1의 pattern 그룹 표에서 "단순 one-way" 그룹에
속한다(`PAIR`, `PUBSUB`, `DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_STREAM`).

- C++ 단순 one-way 목표: **최소 기준 85% / 중앙값(aggregate mean) 목표 95%**
  (완화 목표 선택 시 90%).
- C++/Rust latency 상한: **평균 latency의 C 대비 최대 2.0배**. 계획서
  §2.2의 예외 표에는 C++ `DEALER_DEALER`에 대한 예외가 없다(.NET Single
  `DEALER_DEALER`/`ws`/256B에만 6.0배 예외가 있다) — 표준 2.0배 상한을
  그대로 적용했다.

**Throughput**: aggregate mean **19.02%**는 기본 목표 95%와 완화 목표
90%, 개별 최소 기준 85%를 모두 크게 밑돈다. 6개 크기 전부가 개별 최소
기준(85%) 미달이며, 작은 크기(64B/256B/1024B)는 1% 미만으로 사실상
측정 불능 수준이다.

**Latency**: aggregate mean **0.031배**는 상한 2.0배 이내로 통과(C보다
훨씬 빠르다). 그러나 §3에서 확인했듯 이 값은 진짜 낮은 지연이 아니라
처리량 자체가 초당 약 1만 건으로 상한에 걸려 in-flight 메시지가 거의
없기 때문에 나타나는 부작용으로 보인다 — latency 통과가 이 pattern의
실질적 성능 문제를 상쇄하지 않는다.

**65536B-style dip 여부**: 이번 결과는 `PAIR`/`PUBSUB`에서 관측된 특정
크기(65536B)만의 국소적 하락(page fault storm 환경 지배)과는 패턴이
다르다. 여기서는 **모든 크기**에서 C++ throughput이 약 9,000~10,000
msg/s로 평탄하고, 오히려 상대 비율은 크기가 커질수록 개선된다
(64B 0.37% → 262144B 55.77%) — 이는 국소적 dip이 아니라 pattern
전역에 걸린 고정 상한(약 10K msg/s ≈ 메시지당 약 0.1 ms)의 증거다.
따라서 §6의 "known environment signature" 재확인 절차(`/usr/bin/time -v`
fault count 비교)는 이번 셀에는 적용하지 않는다 — 그 절차는 특정 크기의
allocator/page-fault 국소 dip을 진단하기 위한 것이고, 이번 관측은 모든
크기에 걸친 전역적 상한이라 원인의 성격이 다르다. 이 상한의 근본 원인은
hot path 검토(§6 다음 조치)가 필요한 binding-side 후보로 남긴다 — 진단
결과 없이 "환경 지배"로 단정하지 않는다.

**종합 판정: 미달(throughput aggregate mean 19.02%)** — throughput이
§8의 통과 조건(aggregate mean이 pattern 그룹 목표 충족)을 만족하지
못했으므로 latency가 상한 이내라도 전체 상태는 `통과`가 아니다.

`보류`로 기록하지 않는 이유(§8 규칙): `보류`는 "paired 측정, 자체 개선
pass, Sol 리뷰 기반 두 번째 개선 pass를 완료했지만 public contract를
유지한 추가 개선 요소가 없어 현재 aggregate 목표를 달성하지 못한 채
다음 대상으로 이동"하는 경우에만 쓴다. 이번 측정은 `DEALER_DEALER`/`tcp`
에 대한 **첫 공식 paired 측정**이며, `DEALER_DEALER` 전용 자체 pass나
Sol 리뷰 pass가 아직 수행되지 않았다(§5.1의 C1~C5 부수 확인은 64B 단일
크기·비공식 측정이며 §7.4 순서상 정식 pattern 전용 pass가 아니다).
따라서 계획서 §7.4 순서(자체 pass → Sol 리뷰 pass → 판정)가 아직
`DEALER_DEALER`에 대해 완료되지 않았으므로 `미달(19.02%)`로 기록하고
`보류`로 바꾸지 않는다.

참고: `PAIR`/`tcp`는 `보류(84.92%)`, `PUBSUB`/`tcp`는 `보류(87.76%)`였다
(로그 `log/2026-08-23-cpp-pair-tcp-official.md`,
`log/2026-08-23-cpp-pubsub-tcp-official.md`). `DEALER_DEALER`의 aggregate
throughput **19.02%**는 두 pattern보다 압도적으로 낮다 — 진행 시트
§5의 C1~C5 부수 확인 로그(`log/2026-08-23-cpp-c1-c3-c4-c5-implementation.md`
§4.4)에서도 64B 단일 크기에서 이미 C++가 약 9,791~9,849 msg/s로 관측돼
working tree 변경(C1/C3/C4/C5) 전후로 일관됐다(−0.58%, 잡음 범위) — 즉
이번 공식 측정의 낮은 값은 이번 세션의 회귀가 아니라 `DEALER_DEALER`
binding 자체의 기존 특성이다.

## 6. 다음 조치 (계획 §7.4 순서 참고, 이번 작업 범위 밖)

1. `DEALER_DEALER`/`tcp`에 대한 자체 개선 pass(§7.4)를 수행한다 — 최우선
   진단 대상은 throughput이 메시지 크기와 무관하게 약 9,000~10,000 msg/s로
   평탄한 현상이다(§3~§5). C의 latency 대비 C++ latency가 압도적으로 낮은
   점(0.031배)과 함께 보면 C++ DEALER_DEALER 경로가 파이프라인 없이
   요청 단위로 직렬화되거나(예: 매 send 후 명시적 ack/응답을 기다리는
   경로), `sndtimeo_ms`/`rcvtimeo_ms=200` 관련 polling 주기, 또는
   비동기 admission 경로의 고정 지연이 실제 원인일 가능성이 있다 —
   확정 진단은 이번 작업 범위 밖이며 코드를 수정하지 않았다.
2. 자체 pass 뒤에도 미달이면 Sol에 read-only review를 요청하고, 계약을
   보존하는 후보가 있으면 두 번째 개선 pass를 수행해 after를 다시 공식
   release runtime으로 측정한다.
3. 두 pass를 모두 마쳤는데도 계약 보존 후보가 없으면 §8 규칙에 따라
   `보류`로 pattern 결과를 확정한다.
4. `tcp`의 `DEALER_DEALER`가 확정되면(통과/보류) 계획서 §7.4 순서에 따라
   C `STANDARD_PATTERNS`의 다음 pattern(`DEALER_ROUTER`)으로 진행한다.

## 7. 코드·commit 상태

이번 세션에서 코드는 수정하지 않았다(측정만 수행). commit·push도 수행하지
않았다. working tree는 이전 `PAIR`/`PUBSUB` 공식 측정 시점과 동일한
상태(HEAD `8a5a0361da`, clean)를 유지했다.
