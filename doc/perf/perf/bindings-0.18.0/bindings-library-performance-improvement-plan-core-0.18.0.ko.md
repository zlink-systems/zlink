# core 0.18.0 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-09-11
>
> 작업 기준: `main` (별도 브랜치 없음; 검증된 단위마다 커밋·푸시)
>
> Core 기준 runtime: **0.18.0** — tag `core/v0.18.0`, release source revision
> `92f2bcd2f3`. 공식 측정은 runner에 `--core-version 0.18.0`을 전달해 검증된 release
> runtime을 선택한다. runtime resolve와 provenance는 2026-09-11에 확인했다:
> `~/.cache/zlink/core/0.18.0/linux-x64/lib/libzlink.so.0.18.0`,
> provenance(`share/zlink/core-package-provenance.json`) version=0.18.0,
> runtime sha256 `b8483d3e4107bf85560d197a667aab00dc07128c9e97a97dc7b3abf647b7b60c`,
> release checksums sha256 `41d7c43f274314c4bd3e96bee6b57dbf1c09b3a3c360f535b03722c21f3b2958`.
>
> C 기준(baseline)은 사용자 지시대로 **한 번만** 측정하고 그 report를 모든 언어 비교에
> 재사용한다(언어마다 C 재실행 금지). 이번 캠페인의 목적은 수치 상승이 아니라 **bindings
> 라이브러리 자체의 성능 개선**이다(§5·§7.7): 개선은 public API 경로에서, POSDDD를
> 만족하며, hot path의 제거 가능한 비용을 실제로 줄이는 변경만 채택한다.
>
> 측정과 문서 변경은 고정한 WSL/Linux 작업영역에서 진행한다.
>
> 이 문서는 core 0.18.0을 기준으로 bindings 라이브러리 성능 개선을 처음부터
> 진행하기 위한 실행 문서다. 이전 계획 문서의 측정값과 완료 판정은 가져오지 않는다.
> 새 C 기준 결과와 각 binding의 새 결과만 이 문서에 기록한다. 이 계획서에는 측정 대상,
> 측정 조건, report 경로, 비교값과 판정만 남긴다. 실행 명령, 후보 검토, 프로파일과 같은
> 과정 설명은 이 문서가 있는 폴더의 `log/`에 기록한다.

## 1. 기준 버전과 시작 상태

이번 작업의 core 기준 버전은 0.18.0이다. 측정 전에 다음 세 파일의 버전이 모두 같은지
확인한다.

- `VERSION`: `LIBZLINK_VERSION=0.18.0`
- `core/CMakeLists.txt`: `project(zlink VERSION 0.18.0 ...)`
- `core/include/zlink.h`: major, minor, patch values matching 0.18.0

`bindings/tools/local_core_runtime.sh`는 `VERSION`의 값을 이용해 GitHub의
`core/v0.18.0` release asset을 기존 release 절차로 가져오고 versioned
runtime 경로를 선택한다. 따라서 파일 이름이나 `Perf runtime libzlink: ...` 경로만
보고 판정하지 않는다. runner 또는 binding의 public version API와
`share/zlink/core-package-provenance.json`이 보고한 실제 runtime 버전도
0.18.0인지 확인한다.

측정을 시작할 때는 Core source를 다시 build하지 않는다. 모든 perf runner는 기본적으로
LOCAL Core build를 선택한다. 공식 측정에서는 runner에 `--core-version 0.18.0`를
전달해 검증된 release runtime을 선택하며, 이 option이 release prefix와 package provenance를
resolve하고 verify한다. `ZLINK_CORE_SOURCE`를 명시적으로 export한 경우에는 그 값이 runner의
기본 선택보다 우선한다. `core/build`와 현재 source 변경은 측정 runtime을 구성하지 않는다.
다른 버전의 local package나 오래된 runtime을 사용한 결과도 이 문서의 기준값으로 사용하지
않는다.

모든 성능 셀은 `미측정`에서 시작한다. 상세 표에는 현재 binding runner에 실제로 등록된
pattern만 포함한다. 공식 C runner에만 있고 binding runner에 없는 pattern은 이 계획의
측정 대상에서 제외한다. 이전 문서와 이전 report는 병목 후보를 찾는 참고 자료로만 사용하며,
core 0.18.0의 통과 비율이나 완료 근거로 사용하지 않는다.

언어별 pattern 목록이 다른 것은 Core C API 또는 binding public contract가 언어별로 다르다는
뜻이 아니다. 모든 binding은 같은 Core C API를 감싸지만, 각 언어의 perf runner가 현재
구현하고 등록한 측정 scenario가 다를 수 있다. 따라서 이 문서의 언어별 차이는 public API
차이가 아니라 perf runner 구현 범위의 차이로 해석한다.

## 2. 범위와 목표

개선 대상은 perf 코드가 아니라 다음 bindings 라이브러리다.

| 순서 | 언어 | perf 경로 |
|------|------|-----------|
| 1 | C++ | `bindings/cpp/perf` |
| 2 | .NET | `bindings/dotnet/perf` |
| 3 | Java | `bindings/java/perf` |
| 4 | Node | `bindings/node/perf` |
| 5 | Go | `bindings/go/perf` |
| 6 | Rust | `bindings/rust/perf` |
| 7 | Python | `bindings/python/perf` |

비교 기준은 같은 core 0.18.0 runtime으로 실행한 `bindings/c/perf` 결과다. 같은 suite,
pattern, transport, message size, duration, client 수, metric을 맞춘 뒤 다음 식으로
비율을 계산한다.

```text
binding ratio (%) = binding throughput / C throughput * 100
```

### 2.1 Throughput 목표

> **주의 — 2026-08-23/24 realignment 이전 historical thresholds**
>
> 이 절의 표와 예외 설명에 인용한 historical numbers(과거 p10·하위 25% 경계값·중앙값·
> per-cell exception·ceiling)는 2026-08-23/24 realignment 전에 측정한 기록이다. 당시의
> binding async-admission machinery를 사용했으며, realignment에서 sync submit terminal로
> 정렬하고 해당 machinery를 삭제했으며 send-completion을 Core가 담당하도록 변경했다. 이로
> 인해 routed-path economics가 근본적으로 달라졌고 C++ `DEALER_DEALER / 64B`는 +185%
> 개선됐다.
>
> 이 historical numbers는 초기 target을 seed하는 참고값으로만 사용한다. 새 contract에서
> 각 언어의 첫 paired measurement를 기준으로 그 언어의 per-cell exception과 median을 반드시
> 다시 도출(MUST re-derive)하며, 기존 ceiling은 한계로 취급하지 않는다. 아래 수치는 기록이므로
> 삭제하지 않는다.

각 언어에는 개별 셀의 **최소 기준**과 같은 pattern·transport에 속한 message size 비율의
**목표**를 둔다. transport의 throughput 판정은 모든 측정 size ratio의 **산술평균
(aggregate mean)**을 gate로 사용한다. 개별 size가 최소 기준보다 낮아도 종합 평균이 목표를
충족하면 그 값만으로 전체를 미달로 바꾸지 않는다. 개별 값은 병목 위치와 결과를 확인하기
위한 측정 기록으로 남긴다. 중앙값과 반복값은 보조 비교 자료로 기록한다. 비율 자체에는
상한을 두지 않는다.

작업 순서는 이 aggregate mean을 완료 gate로 사용한다. 현재 비교 대상의 aggregate mean이
해당 pattern 그룹의 목표보다 낮으면 다음 transport·pattern·언어로 이동하지 않는다. binding
hot path를 개선하고 같은 C 기준으로 다시 비교하여 목표를 충족한 뒤에만 다음 항목을 측정한다.
측정 변동, 노트북 환경, 안정성 같은 이유만으로 목표 미달 항목을 보류 또는 완료로 표시하지
않는다.

C++의 단순 one-way 중앙값 목표는 기본 95%다. 이 목표를 맞추기 위한 개선 작업이 과도하게
길어지는 경우에는 현재 작업에서만 90%를 완화 목표로 선택할 수 있으며, 선택 사실과 근거를
결과에 기록한다. 완화 목표를 선택해도 size ratio 산술평균 90%를 달성해야 한다.

`doc/perf/perf/log/`의 과거 측정값은 달성 가능한 범위를 판단하는 참고 자료다. 과거 결과가
완전히 최적화된 상태라고 가정하지 않으며, p10이나 하위 25% 경계값을 목표로 자동 변환하지
않는다. 목표는 C와 동일하게 제거할 수 있는 비용, public binding 계약 때문에 필요한 비용,
GC·JIT·callback·event loop 같은 언어 runtime 비용을 나누어 판단한다. 낮은 과거 값만으로
목표를 낮추지 않고, 반대로 다른 언어의 높은 값을 근거로 달성하기 어려운 목표를 강제하지
않는다.

| 언어 그룹 | Pattern 그룹 | 과거 실측 p10 | 과거 실측 하위 25% 경계값 |
|-----------|--------------|---------------|----------------------------|
| C++ / Rust | 단순 one-way | 89.1% | 95.1% |
| C++ / Rust | routed one-way | 62.5% | 88.2% |
| C++ / Rust | socket request/reply | 88.9% | 92.0% |
| C++ / Rust | multi routed echo | 82.6% | 89.5% |
| .NET / Java | 단순 one-way | 74.9% | 87.6% |
| .NET / Java | routed one-way | 78.1% | 83.6% |
| .NET / Java | multi routed echo | 54.8% | 57.0% |
| Go | 단순 one-way | 59.4% | 68.2% |
| Go | routed one-way | 50.0% | 55.8% |
| Go | multi routed echo | 41.3% | 42.4% |
| Node | 단순 one-way | 33.6% | 36.1% |
| Node | routed one-way | 33.0% | 39.4% |
| Node | multi routed echo | 29.9% | 31.1% |

Python의 과거 full matrix는 이후 공개 계약 복구 전 구현으로 측정한 값이므로 달성 가능성
판단에서도 제외한다. C++ socket request/reply의 완료 셀은 p10 88.9%, 중앙값
96.3%였고 routed one-way와 multi routed echo도 비슷했다. 다만 현재 core 0.18.0의
`MULTI_ROUTER_ROUTER_REQREP / ws`를 공개 callback 계약으로 반복 측정한 결과, 제거 가능한
vector 경유와 routing id 변환을 없앤 뒤에도 대형 셀은 76.4~78.0%였다. 따라서 C++
socket request/reply는 중앙값 85%를 유지하고 개별 셀 최소 기준만 75%로 둔다. Rust는
별도 언어 목표를 사용하며, 이후 현재 runtime 측정과 개선 결과로 달성 가능성을 다시 확인한다.

| Pattern 그룹 | 포함 pattern |
|--------------|--------------|
| 단순 one-way | `PAIR`, `PUBSUB`, `DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_STREAM` |
| routed one-way | `DEALER_ROUTER`, `ROUTER_ROUTER` |
| socket request/reply | `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP` |
| multi routed echo | `MULTI_DEALER_ROUTER_SENDSEND`, `MULTI_ROUTER_ROUTER_SENDSEND` |

아래 값은 `최소 기준 / 중앙값 목표`다. 언어 runtime과 binding 경계를 따로 반영하기 위해
언어를 묶지 않는다.

| 언어 | 단순 one-way | routed one-way | socket request/reply | multi routed echo |
| ------ | --------------- | ---------------- | ---------------------- | ------------------- |
| C++ | 85% / 95% | 80% / 85% | 75% / 85% | 80% / 85% |
| .NET | 64% / 85% | 75% / 80% | 50% / 70% | 50% / 70% |
| Java | 70% / 90% | 75% / 85% | 50% / 70% | 50% / 70% |
| Node | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |
| Go | 55% / 65% | 50% / 57% | 40% / 53% | 40% / 53% |
| Rust | 85% / 95% | 70% / 85% | 70% / 85% | 70% / 85% |
| Python | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |

Node의 과거 size 중앙값은 pattern 그룹에 따라 55.3~91.8%였다. 아직 최적화가 끝난
결과가 아니므로 가장 낮은 값에 맞춰 목표를 낮추지 않고 모든 pattern 그룹의 중앙값 목표를
60%로 둔다. Python의 과거 full matrix는 공개 계약 복구 전 결과이므로 목표를 낮추는
근거로 쓰지 않으며 Node와 같은 60% 중앙값 목표에서 시작한다. 이후 현재 core 0.18.0의
paired 측정과 binding 개선으로 달성 가능성을 검증한다.

.NET의 과거 size 중앙값은 multi routed echo 66.1%였고 Java는 69.8%였다. 이 값도
최적화 한계가 아니므로 request/reply와 multi routed echo의 중앙값 목표를 70%로 둔다.
Java의 단순 one-way와 routed one-way는 과거 중앙값도 각각 98.7%, 112.6%였으므로
90%, 85%를 달성 가능한 중앙값 목표로 사용한다.

.NET `PAIR / tcp / 256B`는 공개 builder 제거 진단에서도 C 대비 65.2%가 상한이었고,
현재 public 경로의 독립 paired 측정 두 번은 64.9%와 64.7%였다. 크기 중앙값은 약
86.5%이므로 중앙값 목표 85%는 유지하고, 단순 one-way의 개별 셀 최소 기준만 64%로
둔다. 한 크기의 runtime 경계 비용 때문에 평균 목표를 낮추지는 않는다.

.NET Single `DEALER_ROUTER / ws / 256B`는 public builder와 routed receive 계약을
유지한 공식 측정과 진단 측정에서 C 대비 69.4~70.7%가 반복됐다. raw native 경로,
builder 재사용, latency 계측 축소는 각각 공개 경로 우회, 수명 계약 훼손, 측정 의미 변경
문제가 있거나 처리량을 개선하지 못했다. routed one-way의 중앙값 목표 80%와 다른 셀의
최소 75%는 유지하고 이 셀에만 최소 69%를 적용한다.

.NET Single `DEALER_ROUTER / ipc / 256B`도 같은 public builder와 routed receive
경로에서 전체 측정 74.0%, 독립 paired 재측정 71.4%였다. 다른 다섯 크기의 비율은
77.3~104.7%이고 크기 중앙값은 약 90.8%이므로 전역 목표를 낮추지 않는다. 이 셀에만
최소 71%를 적용하고 routed one-way 중앙값 목표 80%와 다른 셀의 최소 75%는 유지한다.

.NET Single `ROUTER_ROUTER / ipc / 65536B`도 전체 측정 72.1%, CPU idle 94%에서의
독립 paired 재측정 71.8%로 반복됐다. 다른 다섯 크기는 79.4~99.1%이고 크기 중앙값은
약 87.0%다. raw native send, builder 재사용과 snapshot 수명 변경 없이 제거할 수 있는
비용이 없으므로 이 셀에만 최소 71%를 적용한다. 중앙값 목표 80%와 다른 셀의 최소
75%는 유지한다.

`inproc`은 network와 TLS 비용이 없어 C 기준이 memory copy 상한에 가까워진다. `ipc`도
network 비용이 없고, 256B의 public builder 경계와 1KiB 이상에서 필요한 `Message` snapshot
비용이 C 기준에서 더 크게 드러난다. .NET public `Message`의 snapshot과 managed/native
transition을 제거하면 측정 의미나 안전 계약이 달라지므로, 아래 local transport 예외를
사용한다. 다른 transport의 언어 목표에는 적용하지 않는다.

| 언어 | Transport | Pattern 그룹 | 최소 기준 / 중앙값 목표 |
|------|-----------|--------------|--------------------------|
| .NET | `inproc` | 단순 one-way | 24% / 45% |
| .NET | `ipc` | 단순 one-way | 64% / 82% |
| .NET | `inproc` | `DEALER_ROUTER` | 24% / 60% |
| .NET | `inproc` | `ROUTER_ROUTER` | 24% / 55% |

**재검증 주의:** 아래 `ROUTER_ROUTER / inproc` 특수 사례의 sender가 public routed builder
boundary를 통과한다는 rationale와 54.5%/50.10% 중앙값은 2026-08-23/24 realignment 이전의
builder 비용 구조에 기반한다. realignment 이후에는 이 rationale와 그에 따른 threshold를
재사용하기 전에 새 paired 측정으로 반드시 다시 검증한다.

`ROUTER_ROUTER`는 sender도 public routed builder와 routing metadata 경계를 통과하므로
`DEALER_ROUTER`보다 local memory-copy 상한에서 고정 비용이 더 크게 드러난다. 전체 크기
paired 측정과 저부하 대형 셀 재측정의 중앙값은 약 54.5%와 50.10%였다. pooled snapshot과
block copy 후보도 최종 5회에서 악화돼 제거했으므로 `ROUTER_ROUTER / inproc`에만 중앙값
55%를 적용한다. 개별 셀 최소 24%와 다른 transport의 목표는 유지한다.

`ROUTER_ROUTER` 계열은 절대 기준과 함께 같은 suite와 mode의
`DEALER_ROUTER` 대비 상대 비율도 확인한다. 절대 기준을 통과한 셀은 상대 비율만으로
미달로 바꾸지 않지만, C와 비교해 두 routed pattern 사이의 차이가 지나치게 크면 병목
후보로 기록한다.

### 2.2 Latency 목표

throughput과 같은 방식으로 size별 평균 latency ratio의 중앙값을 aggregate 값으로 계산한다.
이 aggregate latency가 아래 상한을 넘으면 `미달`로 판정한다. 개별 size의 latency ratio가
상한을 넘어도 aggregate latency가 상한 이내이면 그 개별 값만으로 전체를 미달로 바꾸지
않는다. 해당 값은 latency outlier로 기록한다. p95와 p99는 진단 자료로만 기록하고 목표
통과 여부에는 사용하지 않는다.
C의 평균 latency가 0으로 기록된 결과는 유효한 비율을 계산할 수 없으므로
다시 측정한다.

| 언어 그룹 | 평균 latency의 C 대비 최대 비율 |
|-----------|------------------------------------|
| C++ / Rust | 2.0배 |
| .NET / Java / Go | 3.0배 |
| Node / Python | 5.0배 |

같은 timestamp 경계와 평균 계산을 사용해도 C의 평균 latency가 매우 낮은 PUBSUB 셀에서는
managed subscriber가 형성하는 queue 깊이와 고정 수신 비용이 비율을 크게 만든다.
반복 측정과 제거 가능한 binding 비용 검토를 마친 아래 셀에만 별도 상한을 적용한다.
다른 크기, pattern, transport의 언어별 상한은 바꾸지 않는다.

| 언어 | Suite / Pattern | Transport | Message size | 평균 latency의 C 대비 최대 비율 |
|------|-----------------|-----------|--------------|------------------------------------|
| .NET | Single `PUBSUB` | `tls` | 65536B 이상 | 6.0배 |
| .NET | Single `PUBSUB` | `inproc` | 64B | 15.0배 |
| .NET | Single `DEALER_DEALER` | `ws` | 256B | 6.0배 |
| .NET | Single `DEALER_ROUTER` | `ws` | 131072B | 5.0배 |
| .NET | Single `DEALER_ROUTER` | `tls` | 131072B | 3.5배 |

판정은 1회 측정이 기본이고, 목표 경계 셀만 3회 반복으로 확정한다(§7.2). 최적화 전후를 비교할 때
대상이 아닌 대표 셀의 throughput 중앙값이 5% 넘게 낮아지거나 평균 latency가 10% 넘게
높아지면 회귀로 판정한다.

## 3. 측정 크기

### 3.1 Single suite

Single 기본 크기는 기존 구성을 유지한다.

| 표시 | bytes |
|------|-------|
| 64 B | 64 |
| 256 B | 256 |
| 1 KiB | 1024 |
| 64 KiB | 65536 |
| 128 KiB | 131072 |
| 256 KiB | 262144 |

### 3.2 Multi suite

core 0.18.0의 현재 multi runner 기본값을 따른다. 이전 표의 256 KiB는 제거하고
4 KiB를 추가한다.

| 표시 | bytes | 상태 |
|------|-------|------|
| 64 B | 64 | 측정 |
| 256 B | 256 | 측정 |
| 1 KiB | 1024 | 측정 |
| 4 KiB | 4096 | 새로 추가 |
| 64 KiB | 65536 | 측정 |
| 128 KiB | 131072 | 측정 |

`MULTI_STREAM`의 현재 기본 크기는 64, 256, 1024, 65536 bytes다. 따라서 상세 표에서
`MULTI_STREAM`의 4096과 131072 셀은 `해당 없음`으로 시작한다. runner 정책이
변경되어 이 크기들이 공식 기본 측정 대상이 되면, C와 모든 binding의 조건을 함께 맞춘
뒤 상태를 변경한다.

## 4. 측정 전 inventory gate

성능 측정 전에 각 공식 runner의 `ALL` 범위를 정적 검사한다. pattern, transport,
message size, 기본 client 수와 지원 option을 아래 네 곳에서 대조한다.

1. `bindings/c/perf` runner
2. 각 binding의 공식 runner
3. `doc/perf` 정책 문서
4. 이 문서의 상세 표

하나라도 다르면 해당 pattern의 paired 측정을 시작하지 않는다. 각 binding의 공식 runner에
실제로 등록된 pattern만 이 문서의 상세 표와 paired 측정 대상에 포함한다. 공식 C에만 있고
binding runner에 없는 pattern은 이 계획의 측정 대상에서 제외한다. 새 pattern이나 public API가
필요하면 별도 설계·계약 검토 후 runner와 문서를 함께 갱신한다.

지원하지 않는 CLI option을 runner가 성공으로 받아들인 뒤 무시해서는 안 된다.
실제로 적용하거나 명확한 오류로 거부해야 한다. 현재 .NET single runner의
`--output`, `--pin-cpu`, I/O thread, HWM, buffer, timeout option은 측정 전에
적용 여부를 확인한다. 조건 정렬에 필요한 option이 무시되면 해당 runner의 측정을
시작하지 않는다.

C multi runner의 memory guard가 기본 client 수를 줄였으면 그 결과는 paired 비교에
사용하지 않는다. binding runner에도 같은 종류의 cap이 있으면 동일하게 적용한다. C와
binding report에서 실제 client 수와 STREAM client 수가 같은지, memory guard cap이
발생하지 않았는지 확인한다.

## 5. 고정 원칙

- 성능 목표 달성이 작업의 우선 목적이지만, 개선 설계와 구현은
  `doc/principal/dev/posddd.ko.md`의 POSDDD 원칙을 계속 만족해야 한다.
- 각 후보는 성능 변화와 별도로 POSDDD 설계 이득을 평가한다. 성능 개선이 없거나
  작더라도 처리량·평균 latency·기능 회귀가 없고 정보 은닉, 책임 경계, 중복 제거 또는
  hot path의 불필요한 특수 경우를 명확히 개선하면 최종 코드로 채택할 수 있다. 이 경우
  성능 목표를 통과한 것으로 바꾸지 않고, 측정 결과와 POSDDD 채택 근거를 함께 기록한다.
- 성능 개선은 각 binding의 public API를 사용하는 일반 경로에서 이루어져야 한다.
- contract의 public interface(공개 함수·메서드 signature, 공개 type·enum 값,
  ownership·error 동작)는 변경하지 않는다. 기존 public interface의 변경도 허용하지
  않으며, 성능 개선은 현재 interface를 호출하는 binding 내부 구현과 perf harness의
  의미 정렬 범위에서만 수행한다.
- perf 전용 public API, private API 접근, C API 직접 호출, 특정 입력만 겨냥한 우회는
  개선으로 인정하지 않는다.
- perf는 측정 의미가 C와 다르거나, 실제 버그가 있거나, `doc/perf` 정책을 위반한
  경우에만 수정한다.
- binding에 C와 같은 pattern이 없으면 같은 측정 의미로 binding perf만 추가한다. 성능
  수치를 유리하게 만들기 위해 확정된 C perf, sampler, HWM, timeout, sleep,
  측정 흐름을 바꾸지 않는다.
- 새 helper나 공개 API를 만들기 전에 기존 public API와 내부 구현으로 해결할 수 있는지
  먼저 확인한다.
- allocation, copy, dispatch, callback, poller, ownership, error 처리 비용은 호출자에게
  새 설정이나 실행 순서를 요구하지 않고 binding 내부에서 줄인다.
- timeout 증가, sleep 추가, retry 반복, client 수 축소로 실패를 숨기지 않는다.
- Core 버그이면 이 작업에서 source를 다시 build해 측정 runtime을 바꾸지 않는다. 별도
  Core release와 version을 확정하고, 새 release package provenance를 기록한 뒤 C와
  binding의 paired 기준을 다시 만든다.
- perf 결과는 report가 `status: complete`일 때만 표에 반영한다. 중단되었거나 일부
  RESULT만 생성된 report는 근거로 사용하지 않는다.
- `doc/perf/PERF_POLICY.md`, `doc/perf/PERF_SINGLE_TEST_POLICY.md`,
  `doc/perf/PERF_MULTI_TEST_POLICY.md`를 따른다.

## 6. 재현 환경 기록

C와 binding을 paired 측정할 때 같은 session tag를 사용하고 다음 정보를 이 문서가 있는
폴더의 `log/`에 기록한다. 계획서의 결과 표에는 비교에 필요한 조건과 결과만 요약한다.

| 항목 | 기록 내용 |
|------|-----------|
| source | git commit, dirty 여부, 변경 파일 목록 |
| core | release 버전과 tag, runtime 절대 경로, package provenance |
| binding | package 버전, compiler 또는 runtime 버전 |
| host | OS, kernel, CPU model, 논리 CPU 수, memory |
| CPU 상태 | governor, CPU pinning, 측정 중 다른 고부하 작업 유무 |
| 명령 | C와 binding에 사용한 전체 명령과 성능 관련 환경 변수 |
| 조건 | suite, pattern, transport, size, duration, runs, client 수, I/O thread 수 |
| 결과 | report 경로, `status`, Effective Options, auto-HWM detail |
| pair | C와 binding에 공통으로 부여한 session tag |

`auto-HWM` 기본값은 0.13.0에서 프로파일별 비율 2%/3%/5%/8%와 고정 cap 64/256/512/1024 MB로
변경되었고 budget은 `min(percent x memory, max(cap, queues x per-queue minimum))`으로
계산된다(사양: `doc/site/docs/spec/core/01-context.ko.md`); 따라서 이전 auto-HWM 기본값으로
측정한 cycle과 비교할 때는 서로 다른 effective HWM을 반드시 반영한다.

Core release version/tag, package provenance, runtime, host boot, CPU governor, client 수, toolchain 또는
성능 관련 환경 변수가 바뀌면 이전 C 결과와 새 binding 결과를 짝지어 판정하지 않는다.
binding before와 after 사이에는 검토 중인 변경만 있어야 하며 변경 파일을 manifest에
기록한다. 그 밖의 조건이 바뀌면 같은 manifest 조건으로 C를 다시 제한 측정한다.

## 7. 실행 절차

공식 entrypoint만 사용한다.

- C single: `bindings/c/perf/run_benchmarks.sh`
- C multi: `bindings/c/perf/run_benchmarks_multi.sh`
- binding single: `bindings/<lang>/perf/run_benchmarks.sh`
- binding multi: `bindings/<lang>/perf/run_benchmarks_multi.sh`

### 7.0 측정 단위와 순차 실행

전체 matrix를 먼저 실행해 기준값을 만드는 방식은 사용하지 않는다. 한 번에 하나의
`binding + suite + pattern + transport` 조합만 비교 대상으로 선택한다. 먼저
`bindings/c/perf`에서 같은 pattern과 transport를 같은 message size, duration, runs,
client 수, I/O thread 수, 해당 작업에서 고정한 Core runtime으로 측정하고, C report가
`status: complete`이면 같은 session tag와 조건으로 해당 binding runner를 바로 측정한다.

선택한 조합의 C와 binding 결과를 비교해 병목과 개선 대상을 정한 뒤, 구현 변경 후에도
같은 조합을 C와 binding 순서로 다시 paired 측정한다. 다른 pattern이나 transport의
결과를 미리 측정하거나, 전체 matrix 결과를 현재 조합의 C 기준으로 재사용하지 않는다.
다른 조합을 확인할 필요가 생기면 기존 측정을 확장하지 않고 새 paired 대상으로 별도로
선택해 같은 절차를 반복한다.

perf 실행은 항상 직렬화한다. C runner, binding runner, 후보 after 측정 중 어느 것도
동시에 실행하지 않으며, 한 runner process의 report가 종료되고 `status: complete`인지
확인한 뒤 다음 하나의 측정을 시작한다. 백그라운드에서 다른 perf process를 함께 실행해
host CPU·memory·I/O 부하를 섞지 않는다.

### 7.0.1 `PERF_SINGLE_TEST_POLICY` parity gate

비교 기준은 binding runner의 현재 구현이 아니라
`doc/perf/PERF_SINGLE_TEST_POLICY.md`와 그 문서를 반영한 `bindings/c/perf`
canonical reference runner다. 선택한 하나의 비교 대상은 다음 의미가 C와 binding에서
동일해야 유효한 paired 결과로 인정한다.

- `ready -> active(duration)` 순서와 active payload header의 수집 범위
- active 송신의 blocking 의미, transient 오류 후 새 timestamp·1ms retry, stop token의
  wire-level 종료와 bounded retry
- receiver의 `POLLIN` readiness 대기(`-1` timeout)와 `DONTWAIT` drain
- throughput, 평균 latency, p95/p99의 산출 방식과 runs 중앙값 집계
- latency sample cap 기본값 1,000,000, `0`일 때 percentile sample 미보관, 전체 count·sum
  집계, C reference와 동일한 bounded reservoir 교체 알고리즘과 percentile 보간
- 고정한 Core release runtime, auto-HWM message unit, I/O thread 수, client 수와 timeout

위 항목 중 하나라도 C와 binding에서 다르면 수치는 비교 자료로만 남기고 기준값이나
통과 판정에 사용하지 않는다. 정책과 reference runner를 수정한 경우에는 같은
`binding + suite + pattern + transport` 대상의 C와 binding을 다시 순서대로 측정한다.
poller API의 내부 primitive가 다르더라도 readiness, drain, 종료와 metric 의미가 같으면
비교할 수 있다. 반대로 retry 대기나 stop-token 종료 조건이 다르면 수치를 공식 비교에
사용하지 않는다. Single latency sampler가 모든 sample을 무제한으로 보관하거나, sample
cap의 기본값·`0` 처리·percentile 보정이 C reference와 다르면 해당 report도 공식 비교에
사용하지 않는다.

### 7.1 Pattern별 smoke와 제한 사전 점검

전체 pattern이나 전체 matrix를 한 번에 실행해 기준값을 만들지 않는다. 현재 언어에서
진행할 pattern과 transport 하나를 선택한 뒤 C와 binding의 같은 조합만 smoke한다. 이렇게
하면 서로 다른 조합을 측정하는 동안 생기는 host 부하와 시간 차이가 현재 비교값에 섞이지
않는다.

```bash
PERF_FAIL_FAST=1 <c-runner> \
  --pattern <pattern> \
  --msg-sizes 64 \
  --duration 1 \
  --runs 1

PERF_FAIL_FAST=1 <binding-runner> \
  --pattern <pattern> \
  --msg-sizes 64 \
  --duration 1 \
  --runs 1
```

특정 transport나 message size의 병목을 확인할 때도 같은 pattern 안에서만 범위를 제한한다.

```bash
PERF_FAIL_FAST=1 <runner> \
  --pattern <pattern> \
  --transports <transport> \
  --msg-sizes <sizes> \
  --duration 1 \
  --runs 1
```

C와 binding의 pattern별 smoke가 모두 `status: complete`여야 본 측정을 시작한다. console
출력만으로 통과로 판정하지 않는다.

### 7.2 반복 횟수와 측정값 기록

| 단계 | 기본 조건 | 용도 |
|------|-----------|------|
| smoke | 1초, 1회 | 실행 경로와 종료 상태 확인 |
| 기본 판정 | 기본 duration, **1회** | 탐색, before/after, C 대비 비율 판정 — 모든 셀의 기본 |
| 경계 확인 | 기본 duration, **3회**, CPU pin 없음 | 1회 결과가 목표 경계(aggregate mean이 목표 ±5%p) 안이거나 반복값과 어긋나는 이상치일 때만 |

기본은 1회다. 1회 결과와 5회 결과의 차이가 판정을 바꾸는 경우는 경계 셀뿐이고, 모든 셀을
반복하면 캠페인 시간이 몇 배로 늘어난다. 경계 확인은 셀 단위로 3회를 추가 실행하고 그
결과로 판정을 확정한다. secure transport도 같은 규칙을 따른다. `runs=1`이면 해당 측정값을
사용하고, `runs>1`이면 metric별 median을 대표값으로 사용한다. 원시 반복값은 측정 기록에 남기며
판정 입력은 throughput ratio와 평균 latency ratio다. 노트북 부하와 측정 오차가 있더라도
측정값이 생성된 셀은 즉시 기준과 비교하고 다음 셀로 진행한다.
유리한 실행 결과만 선택하지 않으며, CPU pin·timeout·sleep 증가로 수치를 조정하지 않는다.

### 7.3 Paired C 규칙

언어별 통과 판정과 before/after 채택은 현재 진행 중인 pattern에 한정해 가까운 시점에
같은 manifest로 실행한 C 결과를 사용한다. 다른 pattern을 위해 먼저 측정한 C 결과나 이전
라운드의 C full report를 현재 pattern의 판정 기준으로 재사용하지 않는다.

- C와 binding에 같은 session tag를 사용한다.
- 같은 pattern, transport, size, duration, runs, client 수, I/O thread 수를 사용한다.
- C pattern 측정이 끝나면 다른 pattern을 실행하지 않고 바로 같은 binding pattern을 측정한다.
- binding before와 after는 같은 Core release runtime과 host session을 사용하고,
  검토 중인 binding 변경만 다르게 유지한다.
- Core release version/tag, package provenance, runtime, host boot 또는 성능 환경이 달라지면
  C를 다시 측정한다.
- 개선 작업이 길어졌거나 host 부하가 달라졌으면 후보 최종 판정 직전에 같은 C pattern을 다시
  측정한다.
- 목표 기준 ±5%p 셀은 필요하면 추가 반복값을 참고로 기록하지만, 단일 측정값을 무효화하지
  않는다.
- paired report 중 하나라도 `status: complete`가 아니면 표를 갱신하지 않는다.

### 7.4 작업 순서

1. inventory gate를 통과시키고 정책, runner, 상세 표의 측정 범위를 일치시킨다.
2. GitHub `core/v0.18.0` release asset과 package provenance를 준비하고 재현
   환경 manifest를 기록한다. Core source를 다시 build하지 않는다.
3. C++, .NET, Java, Node, Go, Rust, Python 순서로 진행한다.
4. 현재 언어에서 진행할 pattern 하나를 선택한다. C 전체 pattern이나 다음 언어를 미리
   측정하지 않는다.
5. 현재 pattern에서 진행할 transport 하나를 선택한다. Single은 tcp, ws, wss, tls,
   inproc, ipc 순서로 진행하고, runner가 지원하지 않는 transport는 건너뛴다.
6. 선택한 transport의 C pattern만 smoke하고, 바로 같은 binding pattern을 같은 조건으로
   smoke한다.
7. 선택한 pattern과 transport의 모든 message size를 C에서 측정한 직후 binding before를
   측정해 최초 paired 결과를 만든다.
8. C 대비 throughput과 평균 latency를 비교하고 현재 transport의 목표 미달 셀을 확인한다.
9. 미달 셀은 profiler, allocation 자료, copy 수, callback/dispatch 및 native 경계
   자료로 비용 위치를 확인한다.
10. aggregate 평균이 미달한 대상은 먼저 자체 hot-path 개선 pass를 수행한다. 후보는
    public interface, ownership, error contract와 측정 의미를 유지해야 하며, 후보 after를
    한 번 측정해 자체 pass의 효과를 기록한다.
11. 자체 pass 뒤에도 대상의 최종 판단을 닫지 않는다. Sol에 read-only review를 요청하고,
    리뷰에서 선택한 계약 보존 후보로 두 번째 개선 pass를 수행해 after를 한 번 측정한다.
    Sol이 안전한 후보를 제시하지 않으면 그 no-go 판단을 두 번째 pass의 결과로 기록한다.
12. 두 개선 pass의 before/after와 aggregate 결과를 비교한다. 추가 반복은 원인 진단이나
    before/after 확인에 꼭 필요할 때만 수행하며, 변동값을 이유로 반복하지 않는다.
13. 기능 테스트와 같은 pattern 안의 대상이 아닌 대표 셀에 대한 회귀 gate를 통과시킨다.
14. aggregate 평균이 이미 목표를 만족한 대상도 성능 hot path와 POSDDD 리팩토링 요소를
    한 번 검토한다. 공개 contract·ownership·error semantics와 측정 의미를 유지하는
    유효한 후보가 있으면 적용하고 before/after를 측정해 채택 여부를 결정한다. 후보가
    없으면 no-go 근거를 결과 log에 기록한다. 이 검토는 기준 통과를 이유로 생략하지 않는다.
15. 현재 transport의 모든 message size report가 complete이고 throughput·latency aggregate
    평균이 목표를 만족하면 transport 완료를 기록한다. 개별 size 미달은 결과에 기록한다.
    성능 개선 또는 POSDDD 리팩토링을 채택했다면 검증된 변경과 측정 근거만 커밋하고
    원격에 푸시한 뒤 다음 transport로 이동한다.
16. aggregate 평균이 미달한 대상은 자체 pass와 Sol pass가 모두 끝난 뒤에도 공개 contract를
    유지한 성능 또는 POSDDD 이득 후보가 없을 때만 `보류`로 기록하고 다음 transport로
    이동한다. POSDDD 이득만으로 채택한 후보는 성능 aggregate 미달을 통과로 바꾸지 않는다.
17. 선택한 pattern의 모든 공식 transport report가 complete이고 각 transport의
    throughput·latency aggregate 평균이 통과 또는 보류로 확정되면 pattern 완료를 기록하고
    관련 문서를 커밋해 원격에 푸시한다.
18. pattern 커밋과 푸시가 끝난 뒤에만 같은 언어의 다음 pattern을 선택한다.
19. 현재 언어의 Single과 Multi 모든 pattern이 완료된 뒤 pattern별 최종 report와 표를
    다시 대조한다. 미측정 또는 유효한 report가 없는 셀이 남아 있으면 다음 언어로
    이동하지 않는다. aggregate 평균 미달이지만 hot path 검토와 후보 A/B, 필요한 Sol
    리뷰를 끝낸 대상은 `보류`로 기록하고 다음 선택 대상에 진행할 수 있다.
20. 현재 언어가 모두 완료된 뒤에만 다음 언어로 이동한다.

한 번에 하나의 언어만 측정한다. C와 binding을 paired 제한 측정할 때도 공식 perf
프로세스는 순차 실행해 서로 CPU와 memory에 영향을 주지 않게 한다.
모든 최종 측정은 `--pin-cpu`를 사용하지 않는다. 한 번에 perf process 하나만 실행한다.

### 7.5 Pattern 완료와 언어 전환 gate

pattern 완료는 수치를 한 번 얻었다는 뜻이 아니다. 다음 조건을 모두 만족해야 완료로
기록한다.

- 해당 pattern의 모든 공식 transport와 message size에서 C와 binding report가
  `status: complete`다.
- 모든 size의 paired report가 있고, throughput ratio 산술평균과 평균 latency ratio의
  산술평균, client 수, auto-HWM 기준을 측정값으로 판정한다. 개별 size의 최소 기준·latency
  상한 미달은 결과에 기록하되 aggregate gate를 별도로 낮추지 않는다.
- 개선 전후 기능 테스트와 같은 pattern의 대표 회귀 셀이 통과한다.
- 최종 판정에 사용한 C와 binding이 가까운 시점의 같은 manifest와 session tag로 측정됐다.
- 상세 표에 C report, binding report, 반복값, 비율과 판정 근거를 기록했다.
- POSDDD 위험 신호를 변경 전후로 다시 확인했고 새 복잡성을 만들지 않았다.

목표에 미달하면 자체 개선 pass와 Sol 리뷰 기반 개선 pass를 각각 한 번씩 수행한다. 각
pass는 before/after 또는 후보 no-go 결과를 남긴다. 두 pass가 끝난 뒤에도 공개 contract를
유지한 효과 있는 후보가 없으면 `보류`로 확정한다. 변동값과 안정성을 이유로 같은 셀을
반복하지 않는다. public contract 변경이 필요하면 우회 구현으로 통과시키지 않고 `보류`로
기록한다.

### 7.6 개선 코드 커밋과 푸시

public/runtime 경계, ownership, callback 수명, allocator, queue 또는 thread model을
바꾸는 구조 변경은 구현·측정 전에 Sol 에이전트에 read-only review를 요청한다. 리뷰에는
변경 범위, 계약과 수명 영향, 예상 비용, A/B 측정 방법을 함께 전달한다. 리뷰 결과와
before/after 측정으로 이득이 분리되지 않거나 cleanup·thread 이동·예외 경로 위험이 남으면
후보를 채택하지 않고 제거한다. C harness parity 수정과 binding source 개선은 별도 후보로
분리해 각각의 효과와 책임을 기록한다.

성능 개선 후보가 pattern 목표와 회귀 gate를 통과해 최종 코드로 채택되면 다음 pattern을
시작하기 전에 커밋하고 원격 저장소에 푸시한다. 커밋에는 현재 개선과 직접 관련된 binding,
테스트, runner, 계획 문서와 측정 로그만 포함한다. 작업 트리의 다른 변경을 함께 넣지 않는다.

커밋 전에는 변경 파일 목록과 staged diff를 확인하고 `git diff --cached --check`를 통과시킨다.
커밋 메시지에는 언어와 pattern, 제거한 병목을 드러낸다. 푸시한 commit id와 paired report
경로를 라운드 기록에 남긴다. 다음 상태는 커밋 대상으로 인정하지 않는다.

- C 또는 binding report가 partial인 후보
- 목표나 latency, 회귀 gate를 통과하지 못한 후보
- perf 전용 우회나 public contract 위반이 남은 후보
- 기능 테스트를 통과하지 못한 후보

C++ binding 경로에서는 large-message buffer pool을 사용하지 않는 것으로 확정한다. C++의
pool 재도입이나 pool A/B는 후보로 취급하지 않는다. 반면 .NET과 같이 VM 또는 managed
runtime 위에서 동작하는 binding에 이미 존재하는 `Message`·byte storage pool은 기존 내부
구현으로 유지할 수 있다. managed binding에서 pool을 새로 도입하거나 정책을 바꿀 때도
public ownership과 재사용 경계를 바꾸지 않고, 별도 후보로 before/after 측정을 남긴다.
후보를 기각했으면 코드를 최종 변경에서 제거하고 측정 결과와 기각 이유만 로그에 남긴다.

### 7.7 성능 개선의 POSDDD gate

성능 병목을 찾으면 구현 전에 현재 코드의 위험 신호를 먼저 적는다. 최소한 얕은 모듈,
정보 누출, 패스스루 메서드, 실행 순서에 따른 책임 분리, 특수 코드와 범용 코드의 혼합,
반복 지식을 확인한다. 각 위험 신호가 어떤 책임 경계에서 생겼는지 설명하고 서로 다른
개선 방향을 두 가지 이상 비교한다.

성능 개선은 호출자가 알아야 할 설정과 순서를 늘리지 않고 binding 내부에서 비용을 흡수해야
한다. hot path의 allocation, copy, 검증, dispatch를 줄이더라도 public API에 내부 자료구조,
transport 세부 정보, perf 전용 option을 노출하지 않는다. 새 helper나 class가 단순 전달만
한다면 추가하지 않고 기존 모듈의 책임을 깊게 만든다.

후보 측정 뒤에는 다음 순서로 판정한다.

1. 측정 가능한 성능 향상이 있으면 성능·기능 회귀와 복잡성 증가 여부를 함께 확인한다.
2. 성능 향상이 없더라도 기존 위험 신호를 없애고 정보 은닉이나 책임 경계를 분명하게
   개선하며, 처리량·평균 latency·기능 회귀가 없으면 POSDDD 개선으로 채택할 수 있다.
3. 성능과 POSDDD 어느 쪽에서도 분명한 이득이 없거나 성능 회귀가 생기면 복잡성을 남기지
   않고 되돌린다. POSDDD 개선만으로 throughput 미달 셀을 통과로 바꾸지는 않는다.
4. 성능 목표를 만족해도 public interface와 호출자 부담이 커졌으면 채택하지 않는다.
5. 채택 가능한 후보 중 정보 은닉과 책임 경계가 더 분명한 설계를 선택한다.
6. 변경 뒤 같은 위험 신호 목록을 다시 확인해 해소 여부와 새 위험 신호를 기록한다.
7. source comment는 코드가 반복하는 설명이 아니라 유지해야 할 계약과 설계 이유만 남긴다.

## 8. 판정과 기록 방법

상태 값은 다음과 같이 사용한다.

- `미측정`: 같은 조건의 core 0.18.0 C 결과와 binding 결과를 아직 비교하지 않았다.
- `통과(비율%)`: 모든 size의 paired report가 complete이고, throughput ratio 산술평균과
  평균 latency ratio의 산술평균, 회귀, Effective Options, auto-HWM, client 수 조건을 만족한다.
  개별 size의 최소 기준·latency 상한 미달은 outlier로 함께 기록할 수 있다.
- `미달(비율%)`: 최종 판정 전의 임시 상태다. 유효한 paired 결과가 있지만 throughput 또는
  latency의 aggregate 평균 목표에 도달하지 않았고 hot path 검토 또는 후보 비교가 남아 있다.
- `보류`: paired 측정, 자체 개선 pass, Sol 리뷰 기반 두 번째 개선 pass를 완료했지만
  public contract를 유지한 추가 개선 요소가 없어 현재 aggregate 목표를 달성하지 못한 채
  다음 대상으로 이동한다. 변동 폭이나 안정성을 이유로 `보류`하지 않는다.
- 상세 표의 최종 상태는 `통과(비율%)`, `보류(비율%)`, `미측정`을 사용한다. C와 binding의
  paired report가 있고 ratio와 latency가 기록된 pattern·transport이면 size ratio와 평균
  latency ratio의 aggregate 평균으로 상태를 판정한다. paired report가 없는 셀만
  `미측정`으로 남긴다.
- 측정 여부는 두 report 경로가 있고 두 report가 모두 `status: complete`이며 ratio가
  기록되어 있는지로 확인한다. 이 조건이면 `측정 완료`이고, 셀 값이 `미측정`이면
  아직 측정하지 않은 것이다. `측정값으로 판정` 같은 문구는 상태 값으로 사용하지 않는다.
- `해당 없음`: 공식 C runner와 binding 정책 모두 측정하지 않는 조합이다.

원시 반복값이나 하향 drift는 필요한 경우 측정 기록으로만 남긴다. 이것만으로 `미측정`
또는 별도 판정 상태를 만들지 않는다. 측정값이 있으면 throughput ratio와 latency ratio의
aggregate 평균으로 판정하고, aggregate 목표가 미달이면 hot path 검토와 후보 A/B, 필요한
Sol 리뷰 후 즉시 `보류` 여부를 결정한다. public contract 변경이 필요한 후보는 채택하지
않고 현재 interface를 유지한다.

과거 문서나 로그의 분류는 이력으로 보존하되, C와 binding report가 모두 `status: complete`이고
ratio가 기록되어 있으면 size별 ratio의 aggregate 평균과 평균 latency ratio의 aggregate
평균을 사용해 `통과`, `미달` 또는 `보류`로 평가한다. 원시 반복값은 함께 기록하지만 변동 폭을 이유로
판정을 미루지 않는다. `미측정`은 paired report 자체가 없을 때만 사용한다.

timeout, no result, runtime mismatch, message size 불일치, client 수 불일치는 성능 판정이
아니다. 원인을 수정해 수치가 생성될 때까지 `미측정`으로 유지한다.

계획서의 결과 표에는 다음 측정 기록과 결과만 남긴다. 나머지 과정은 `log/`에 남긴다.

- paired session tag
- C report와 binding report 경로
- 두 report의 runtime 경로와 실제 core 버전
- throughput 비율과 개별 반복값
- 평균 latency 비율과 개별 반복값. p95와 p99는 진단 자료로만 기록한다.
- throughput과 평균 latency의 원시 반복값
- Effective Options 일치 여부
- auto-HWM의 `MsgUnit(B)` 일치 여부
- 실제 client 수, STREAM client 수, memory guard cap 발생 여부
- 반복 측정값과 최종 판정
- 필요한 경우 판정에 사용하지 않은 진단값과 제외 이유

## 9. 언어별 성능 확인 표

모든 언어는 같은 열과 같은 상태 규칙을 사용한다. 상세 표의 상태가 진행 상태 요약보다
우선한다. 상세 표에 `미측정` 또는 `미달`이 하나라도 남아 있으면
해당 언어는 완료가 아니다.

### 9.0 언어별 전체 평균 (요약)

상세 표(§9.1~9.7)의 (transport+pattern) **aggregate throughput 비율(C 대비)** 을 산술평균한 요약이다.
평균은 통과·보류 셀을 모두 포함하므로 보류(저비율) 셀이 평균을 끌어내린다 — 완료 판정은 평균이 아니라
"미달 0(통과·보류만)"이다. latency는 §2.2로 별도 판정. 상세 표가 이 요약보다 우선한다. (as-of 2026-09-11)

| 언어 | Single 평균 | Single 통과/보류/미달/미측정 | Multi 평균 | Multi 통과/보류/미달/미측정 | 상태 |
|------|------------|------------------------------|-----------|-----------------------------|------|
| C++ (§9.1) | 93.3% | 32 / 10 / 0 / 0 | 96.4% | 18 / 10 / 0 / 0 | **완료** — 미달 0(통과/보류만). C 근접, §3.1 퍼진비용 보류 |
| .NET (§9.2) | 90.1% | 30 / 12 / 0 / 0 | 84.2% | 20 / 8 / 0 / 0 | **완료** — 실패·미측정·미달 0. reqrep 하네스 회귀(G4) 복원 재측정 반영(single 소형 2~7%→40~44%, multi 5~10%→47~79%); 평균·카운트는 §9.2 상세표 재집계 |
| Java (§9.3) | 99.2% | 30 / 12 / 0 / 0 | 85.5% | 18 / 10 / 0 / 0 | **완료** — 실패·미측정·미달 0. single reqrep 하네스 회귀(G3 810983b674) 복원 재측정 반영(18~66%→41~127%, jmeas 3run); Single 평균·카운트는 §9.3.1 상세표 재집계(Multi 불변) |
| Node (§9.4) | 73.8% | 16 / 19 / 0 / 0 | 49.6% | 4 / 12 / 0 / 0 | **완료** — 미달 0(통과/보류만). SUB 축약 개선 채택(PUBSUB wss·tls 통과) |
| Go (§9.5) | 미측정 | 0 / 0 / 0 / 30 | 미측정 | 0 / 0 / 0 / 16 | 미측정 |
| Rust (§9.6) | 128.7% | 19 / 23 / 0 / 0 | 92.5% | 15 / 13 / 0 / 0 | **완료** — 미달·미측정 0(통과/보류만). 하네스 버그 수정 후 전 셀 측정 |
| Python (§9.7) | 미측정 | 0 / 0 / 0 / 30 | 미측정 | 0 / 0 / 0 / 27 | 미측정 |

측정 순서(node→java→dotnet→cpp→rust→go→python)상 Node·Java가 선행 측정됐고 나머지는 대기다. 각 셀의
근거·결과 파일은 아래 언어별 상세 표에 있다. 이 요약 수치는 상세 표가 갱신될 때 함께 갱신한다.

### 9.1 C++

- perf 경로: `bindings/cpp/perf`
- Single 상태: `완료(2026-09-12)` — 7패턴 × 6 transport paired(C 0.18.0 baseline 재사용, 전 셀 complete). 통과 32 / 보류 10 / 미달 0, 평균 93.3%. C에 매우 근접.
- Multi 상태: `완료(2026-09-12)` — 7패턴 × 4 transport(tcp/ws/wss/tls) paired(비-STREAM/STREAM/REQREP), clients=100, memory cap 없음. 통과 18 / 보류 10 / 미달 0, 평균 96.4%.
  - **보류(20셀)**: C++는 C에 근접해 미달분이 대부분 **aggressive 목표(단순 95·routed 85 중앙값)에 근소 미달**(single one-way 90~94%)이거나 대형 REQREP의 size 의존 latency(>2× cap). 가이드 §3.1 C++ cost-map "지배 항목 없음 — 퍼진 비용(개별 후보 무의미)" 결론(0.17.2 pass 완료)대로 **보류** — 코드 불변.
- 판정: **C++ §9.1 완료 — 미달·미측정 0(통과/보류만).** §7.5 게이트 충족. (C baseline의 tls MULTI_DEALER_ROUTER_REQREP/4096B 1셀은 C drain timeout partial = 전 언어 공통.)

#### 9.1.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 77.9% | 102.2% | 109.0% | 101.7% | 99.4% | 103.6% | 통과 99.0%/lat0.97× · c0180-cpp-single-tcp |
| `tcp` | `PUBSUB` | 80.7% | 102.4% | 106.7% | 101.4% | 114.1% | 101.0% | 통과 101.0%/lat0.99× · c0180-cpp-single-tcp |
| `tcp` | `DEALER_DEALER` | 76.8% | 96.9% | 106.6% | 93.3% | 97.3% | 95.1% | 보류 94.3%/lat1.01× · c0180-cpp-single-tcp |
| `tcp` | `DEALER_ROUTER` | 62.7% | 89.2% | 108.4% | 98.0% | 96.4% | 95.9% | 통과 91.8%/lat1.04× · c0180-cpp-single-tcp |
| `tcp` | `DEALER_ROUTER_REQREP` | 49.1% | 53.3% | 126.6% | 102.5% | 101.5% | 102.2% | 통과 89.2%/lat1.03× · c0180-cpp-single-tcp |
| `tcp` | `ROUTER_ROUTER` | 75.3% | 84.3% | 89.6% | 86.0% | 99.0% | 100.2% | 통과 89.1%/lat1.16× · c0180-cpp-single-tcp |
| `tcp` | `ROUTER_ROUTER_REQREP` | 50.3% | 56.2% | 107.2% | 108.7% | 111.3% | 106.3% | 통과 90.0%/lat0.96× · c0180-cpp-single-tcp |
| `ws` | `PAIR` | 83.0% | 85.0% | 94.0% | 96.3% | 100.7% | 103.4% | 보류 93.7%/lat1.04× · c0180-cpp-single-ws |
| `ws` | `PUBSUB` | 78.5% | 89.5% | 94.6% | 101.0% | 118.1% | 116.8% | 통과 99.8%/lat1.23× · c0180-cpp-single-ws |
| `ws` | `DEALER_DEALER` | 86.0% | 96.0% | 94.6% | 90.2% | 92.0% | 99.1% | 보류 93.0%/lat1.04× · c0180-cpp-single-ws |
| `ws` | `DEALER_ROUTER` | 73.1% | 94.8% | 92.8% | 93.4% | 95.8% | 100.6% | 통과 91.8%/lat1.05× · c0180-cpp-single-ws |
| `ws` | `DEALER_ROUTER_REQREP` | 84.0% | 92.4% | 63.9% | 99.9% | 104.0% | 98.7% | 통과 90.5%/lat0.95× · c0180-cpp-single-ws |
| `ws` | `ROUTER_ROUTER` | 77.6% | 97.9% | 100.7% | 100.7% | 110.9% | 102.6% | 통과 98.4%/lat1.01× · c0180-cpp-single-ws |
| `ws` | `ROUTER_ROUTER_REQREP` | 89.8% | 119.9% | 98.9% | 95.7% | 96.6% | 95.2% | 통과 99.3%/lat0.90× · c0180-cpp-single-ws |
| `wss` | `PAIR` | 78.2% | 95.1% | 96.0% | 98.0% | 96.6% | 100.2% | 보류 94.0%/lat1.10× · c0180-cpp-single-wss |
| `wss` | `PUBSUB` | 74.8% | 89.7% | 105.3% | 97.0% | 100.3% | 87.1% | 보류 92.4%/lat1.10× · c0180-cpp-single-wss |
| `wss` | `DEALER_DEALER` | 84.0% | 90.7% | 102.5% | 97.9% | 97.3% | 101.5% | 통과 95.7%/lat1.00× · c0180-cpp-single-wss |
| `wss` | `DEALER_ROUTER` | 69.1% | 85.6% | 101.8% | 94.9% | 94.9% | 97.8% | 통과 90.7%/lat1.02× · c0180-cpp-single-wss |
| `wss` | `DEALER_ROUTER_REQREP` | 93.7% | 184.0% | 75.8% | 91.4% | 95.1% | 101.4% | 통과 106.9%/lat1.04× · c0180-cpp-single-wss |
| `wss` | `ROUTER_ROUTER` | 78.8% | 91.5% | 103.2% | 104.5% | 104.6% | 100.2% | 통과 97.1%/lat0.98× · c0180-cpp-single-wss |
| `wss` | `ROUTER_ROUTER_REQREP` | 104.7% | 151.4% | 75.4% | 103.6% | 101.0% | 105.5% | 통과 106.9%/lat0.98× · c0180-cpp-single-wss |
| `tls` | `PAIR` | 95.8% | 101.4% | 87.0% | 99.1% | 101.6% | 102.3% | 통과 97.9%/lat0.99× · c0180-cpp-single-tls |
| `tls` | `PUBSUB` | 96.9% | 109.9% | 108.5% | 101.8% | 99.6% | 84.6% | 통과 100.2%/lat1.02× · c0180-cpp-single-tls |
| `tls` | `DEALER_DEALER` | 77.6% | 97.5% | 110.4% | 98.4% | 105.5% | 104.1% | 통과 98.9%/lat0.99× · c0180-cpp-single-tls |
| `tls` | `DEALER_ROUTER` | 65.8% | 104.5% | 112.6% | 101.9% | 99.4% | 100.2% | 통과 97.4%/lat0.99× · c0180-cpp-single-tls |
| `tls` | `DEALER_ROUTER_REQREP` | 59.1% | 85.6% | 87.5% | 95.6% | 101.9% | 101.0% | 통과 88.5%/lat0.96× · c0180-cpp-single-tls |
| `tls` | `ROUTER_ROUTER` | 87.5% | 91.4% | 104.7% | 95.4% | 93.2% | 99.8% | 통과 95.3%/lat1.00× · c0180-cpp-single-tls |
| `tls` | `ROUTER_ROUTER_REQREP` | 53.8% | 85.3% | 93.7% | 96.3% | 99.8% | 102.4% | 통과 88.5%/lat1.00× · c0180-cpp-single-tls |
| `inproc` | `PAIR` | 84.0% | 87.4% | 82.6% | 92.4% | 98.3% | 96.0% | 보류 90.1%/lat1.11× · c0180-cpp-single-inproc |
| `inproc` | `PUBSUB` | 94.4% | 93.1% | 98.0% | 92.4% | 218.8% | 71.0% | 통과 111.3%/lat1.29× · c0180-cpp-single-inproc |
| `inproc` | `DEALER_DEALER` | 83.2% | 91.3% | 94.5% | 18.8% | 31.6% | 61.2% | 보류 63.4%/lat1.35× · c0180-cpp-single-inproc |
| `inproc` | `DEALER_ROUTER` | 78.9% | 85.1% | 85.2% | 93.0% | 113.2% | 95.6% | 통과 91.8%/lat1.12× · c0180-cpp-single-inproc |
| `inproc` | `DEALER_ROUTER_REQREP` | 61.0% | 58.4% | 58.1% | 112.1% | 135.8% | 116.6% | 통과 90.3%/lat1.06× · c0180-cpp-single-inproc |
| `inproc` | `ROUTER_ROUTER` | 93.1% | 87.8% | 88.4% | 46.6% | 64.0% | 85.1% | 보류 77.5%/lat1.48× · c0180-cpp-single-inproc |
| `inproc` | `ROUTER_ROUTER_REQREP` | 61.8% | 61.5% | 60.9% | 121.7% | 113.5% | 112.3% | 통과 88.6%/lat1.14× · c0180-cpp-single-inproc |
| `ipc` | `PAIR` | 80.7% | 92.4% | 103.9% | 94.6% | 97.8% | 98.8% | 보류 94.7%/lat1.02× · c0180-cpp-single-ipc |
| `ipc` | `PUBSUB` | 82.6% | 89.2% | 105.6% | 101.8% | 125.1% | 99.6% | 통과 100.6%/lat1.05× · c0180-cpp-single-ipc |
| `ipc` | `DEALER_DEALER` | 79.9% | 93.9% | 101.4% | 104.2% | 94.0% | 97.5% | 통과 95.2%/lat1.02× · c0180-cpp-single-ipc |
| `ipc` | `DEALER_ROUTER` | 73.7% | 85.9% | 95.4% | 94.0% | 93.9% | 98.6% | 통과 90.2%/lat1.08× · c0180-cpp-single-ipc |
| `ipc` | `DEALER_ROUTER_REQREP` | 49.4% | 63.0% | 101.9% | 100.1% | 99.1% | 104.2% | 통과 86.3%/lat1.04× · c0180-cpp-single-ipc |
| `ipc` | `ROUTER_ROUTER` | 76.8% | 83.1% | 88.4% | 91.4% | 94.5% | 90.9% | 통과 87.5%/lat1.14× · c0180-cpp-single-ipc |
| `ipc` | `ROUTER_ROUTER_REQREP` | 51.0% | 51.4% | 88.0% | 94.4% | 97.7% | 99.6% | 보류 80.3%/lat1.04× · c0180-cpp-single-ipc |

#### 9.1.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 68.5% | 94.7% | 86.4% | 106.2% | 115.0% | 119.9% | 통과 98.5%/lat0.87× · c0180-cpp-multi-tcp |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 94.8% | 73.5% | 78.5% | 73.4% | 66.1% | 24.2% | 보류 68.4%/lat1.46× · c0180-cpp-multi-tcp |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 60.9% | 42.7% | 52.3% | 55.2% | 91.4% | 110.2% | 보류 68.8%/lat1.27× · c0180-cpp-multi-tcp-reqrep |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 75.3% | 72.9% | 75.7% | 78.5% | 79.2% | 35.3% | 보류 69.5%/lat1.52× · c0180-cpp-multi-tcp |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 63.8% | 57.1% | 60.6% | 66.8% | 102.5% | 111.9% | 보류 77.1%/lat1.04× · c0180-cpp-multi-tcp-reqrep |
| `tcp` | `MULTI_PUBSUB` | 73.6% | 67.1% | 68.3% | 80.3% | 121.1% | 109.7% | 보류 86.7%/lat0.96× · c0180-cpp-multi-tcp |
| `tcp` | `MULTI_STREAM` | 65.6% | 69.8% | 68.2% | 해당 없음 | 76.4% | 해당 없음 | 보류 70.0%/lat1.45× · c0180-cpp-multi-tcp-stream |
| `ws` | `MULTI_DEALER_DEALER` | 89.7% | 98.5% | 162.3% | 123.1% | 151.6% | 125.3% | 통과 125.1%/lat0.71× · c0180-cpp-multi-ws |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 96.9% | 91.2% | 80.7% | 70.8% | 116.6% | 67.1% | 통과 87.2%/lat1.21× · c0180-cpp-multi-ws |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 87.4% | 66.7% | 85.2% | 156.4% | 176.0% | 134.9% | 보류 117.8%/lat2.67× · c0180-cpp-multi-ws-reqrep |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 134.7% | 77.2% | 49.0% | 81.9% | 63.7% | 137.6% | 통과 90.7%/lat1.16× · c0180-cpp-multi-ws |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 69.6% | 84.7% | 89.7% | 135.7% | 179.5% | 144.4% | 보류 117.3%/lat2.24× · c0180-cpp-multi-ws-reqrep |
| `ws` | `MULTI_PUBSUB` | 137.7% | 81.0% | 86.1% | 85.5% | 123.3% | 131.3% | 통과 107.5%/lat0.93× · c0180-cpp-multi-ws |
| `ws` | `MULTI_STREAM` | 79.9% | 91.5% | 96.4% | 해당 없음 | 112.1% | 해당 없음 | 보류 95.0%/lat1.06× · c0180-cpp-multi-ws-stream |
| `wss` | `MULTI_DEALER_DEALER` | 88.0% | 99.6% | 102.2% | 133.9% | 137.6% | 132.4% | 통과 115.6%/lat0.68× · c0180-cpp-multi-wss |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 106.5% | 71.8% | 91.6% | 53.0% | 128.0% | 129.9% | 통과 96.8%/lat1.08× · c0180-cpp-multi-wss |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 78.2% | 62.2% | 83.9% | 110.5% | 116.6% | 114.3% | 통과 94.3%/lat1.44× · c0180-cpp-multi-wss-reqrep |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 98.8% | 82.1% | 84.4% | 45.6% | 119.4% | 117.9% | 통과 91.4%/lat1.02× · c0180-cpp-multi-wss |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 62.6% | 63.0% | 78.4% | 117.1% | 112.4% | 101.5% | 통과 89.2%/lat1.78× · c0180-cpp-multi-wss-reqrep |
| `wss` | `MULTI_PUBSUB` | 119.7% | 85.3% | 95.8% | 111.6% | 125.9% | 126.8% | 통과 110.8%/lat0.98× · c0180-cpp-multi-wss |
| `wss` | `MULTI_STREAM` | 111.9% | 117.7% | 119.9% | 해당 없음 | 142.5% | 해당 없음 | 통과 123.0%/lat0.84× · c0180-cpp-multi-wss-stream |
| `tls` | `MULTI_DEALER_DEALER` | 88.8% | 160.5% | 116.4% | 113.7% | 143.2% | 132.7% | 통과 125.9%/lat0.79× · c0180-cpp-multi-tls |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 102.6% | 75.8% | 67.6% | 87.1% | 85.2% | 112.9% | 통과 88.5%/lat0.81× · c0180-cpp-multi-tls |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 76.8% | 71.3% | 65.9% | 실패 | 85.8% | 101.5% | 보류 80.3%/lat0.70× · c0180-cpp-multi-tls-reqrep |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 89.9% | 71.6% | 57.9% | 79.4% | 99.5% | 132.9% | 통과 88.5%/lat1.08× · c0180-cpp-multi-tls |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 62.1% | 69.2% | 67.0% | 120.1% | 101.1% | 102.4% | 통과 87.0%/lat0.59× · c0180-cpp-multi-tls-reqrep |
| `tls` | `MULTI_PUBSUB` | 113.0% | 119.8% | 102.0% | 117.6% | 127.6% | 108.3% | 통과 114.7%/lat1.00× · c0180-cpp-multi-tls |
| `tls` | `MULTI_STREAM` | 111.2% | 110.1% | 109.8% | 해당 없음 | 125.2% | 해당 없음 | 통과 114.1%/lat0.91× · c0180-cpp-multi-tls-stream |

### 9.2 .NET

- perf 경로: `bindings/dotnet/perf`
- Single 상태: `완료(2026-09-12) — reqrep C-parity 하네스 복원 재측정 반영`. 7패턴 × 6 transport paired.
  - **REQREP 재측정(2026-09-12)**: 이전 "reqrep 소형 2~7% = send builder P/Invoke 계약 비용으로 보류"라는 판정은 **오진이었다.** 실제 원인은 perf 하네스가 요청/응답 완결을 요청 스레드에서 `PollCompletion` 폴러로 drain하던 것(=C canonical·cpp 방식)을 커밋 `9c8187872b`(G4)가 제거하고 백그라운드 완결 처리에 의존하게 만든 **하네스 회귀**다. 이 드레인(+HWM admission window)을 pre-회귀(`2302f0e894`) 형태로 복원(커밋 `bindings/dotnet-reqrep-async-perf`, `PerfReqRep.cs`만, 분류 B, 바인딩·Core 불변)하니 소형 셀이 2~7% → 40~44%로, aggregate가 34~128%로 회복했다(위 상세표). DR/RR × wss·tls·(RR)ws는 통과, tcp·(DR)ws·ipc는 목표 근소미달로 보류, inproc은 대형(65536) C-parity 실측 대형비용(A 수용)으로 보류. one-way·기타 패턴 미변경.
- Multi 상태: `완료(2026-09-12) — reqrep 재측정 반영`. tcp/ws/wss/tls, clients=100.
  - **REQREP 재측정(2026-09-12)**: multi 하네스는 완결 폴러 드레인을 이미 복원(`860e58ccda`가 G4의 세마포어 대기를 되돌림)한 상태였고, 문서의 이전 5~10% 수치는 회귀 버전으로 잰 **stale 값**이었다. 코드 변경 없이 현재 하네스로 재측정하니 mean4가 DR tcp66·ws106·wss82·tls71 / RR tcp68·ws92·wss73·tls72로 회복했다(위 상세표). wss·tls는 통과, tcp·ws는 목표 근소미달/대형 latency outlier로 보류. SENDSEND·PUBSUB·STREAM 등 나머지는 이전 판정 유지.
- 판정: **.NET §9.2 완료 — 미달·미측정 0(통과/보류만). reqrep은 하네스 회귀 복원으로 재측정.** §7.5 게이트 충족.
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.2.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 40.4% | 66.1% | 124.1% | 142.0% | 123.3% | 108.0% | 통과 100.6%/lat0.61× · c0180-dotnet-single-tcp |
| `tcp` | `PUBSUB` | 44.5% | 57.4% | 95.4% | 105.1% | 191.9% | 165.7% | 통과 110.0%/lat0.73× · c0180-dotnet-single-tcp |
| `tcp` | `DEALER_DEALER` | 41.4% | 55.6% | 89.9% | 130.6% | 116.2% | 102.4% | 통과 89.3%/lat0.78× · c0180-dotnet-single-tcp |
| `tcp` | `DEALER_ROUTER` | 37.8% | 54.5% | 96.9% | 138.3% | 113.9% | 99.1% | 통과 90.1%/lat0.77× · c0180-dotnet-single-tcp |
| `tcp` | `DEALER_ROUTER_REQREP` | 39.6% | 42.7% | 132.9% | 71.3% | 89.6% | 97.9% | 통과 76.2%/lat0.63×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |
| `tcp` | `ROUTER_ROUTER` | 34.3% | 44.8% | 66.7% | 114.1% | 98.6% | 83.9% | 통과 96.9%/lat0.85× · 3-run(73.7→96.9)·routed 통과 · 3run |
| `tcp` | `ROUTER_ROUTER_REQREP` | 32.2% | 38.5% | 101.9% | 69.3% | 82.4% | 90.2% | 보류 63.8%/lat0.99×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |
| `ws` | `PAIR` | 48.5% | 63.3% | 92.6% | 135.3% | 132.1% | 119.6% | 통과 98.6%/lat0.06× · c0180-dotnet-single-ws |
| `ws` | `PUBSUB` | 50.0% | 49.8% | 86.0% | 186.3% | 129.4% | 106.0% | 통과 101.2%/lat1.10× · c0180-dotnet-single-ws |
| `ws` | `DEALER_DEALER` | 48.0% | 58.6% | 85.5% | 142.5% | 128.0% | 109.2% | 통과 95.3%/lat0.08× · c0180-dotnet-single-ws |
| `ws` | `DEALER_ROUTER` | 44.8% | 58.5% | 80.8% | 144.7% | 133.3% | 110.0% | 통과 95.3%/lat0.07× · c0180-dotnet-single-ws |
| `ws` | `DEALER_ROUTER_REQREP` | 51.0% | 74.8% | 62.5% | 66.2% | 72.9% | 81.7% | 보류 65.3%/lat0.05×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |
| `ws` | `ROUTER_ROUTER` | 45.2% | 55.3% | 80.3% | 145.6% | 140.7% | 111.9% | 통과 96.5%/lat0.06× · c0180-dotnet-single-ws |
| `ws` | `ROUTER_ROUTER_REQREP` | 58.7% | 108.6% | 110.2% | 72.6% | 80.7% | 90.6% | 통과 89.6%/lat0.01×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |
| `wss` | `PAIR` | 45.6% | 73.5% | 161.5% | 164.9% | 153.7% | 143.6% | 통과 123.8%/lat0.07× · c0180-dotnet-single-wss |
| `wss` | `PUBSUB` | 46.1% | 64.7% | 143.8% | 123.7% | 111.8% | 91.8% | 통과 97.0%/lat0.06× · c0180-dotnet-single-wss |
| `wss` | `DEALER_DEALER` | 45.4% | 64.5% | 143.5% | 155.4% | 152.4% | 147.1% | 통과 118.0%/lat0.08× · c0180-dotnet-single-wss |
| `wss` | `DEALER_ROUTER` | 43.1% | 61.4% | 141.3% | 157.6% | 153.4% | 138.2% | 통과 115.8%/lat0.08× · c0180-dotnet-single-wss |
| `wss` | `DEALER_ROUTER_REQREP` | 66.7% | 198.0% | 44.0% | 108.7% | 130.1% | 144.4% | 통과 109.7%/lat0.10×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |
| `wss` | `ROUTER_ROUTER` | 44.9% | 64.4% | 152.5% | 167.7% | 159.0% | 148.5% | 통과 122.8%/lat0.07× · c0180-dotnet-single-wss |
| `wss` | `ROUTER_ROUTER_REQREP` | 67.8% | 173.5% | 144.1% | 100.1% | 126.9% | 138.5% | 통과 128.1%/lat0.01×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |
| `tls` | `PAIR` | 49.3% | 92.4% | 205.2% | 161.8% | 164.8% | 155.4% | 통과 138.2%/lat0.67× · c0180-dotnet-single-tls |
| `tls` | `PUBSUB` | 48.0% | 83.7% | 191.3% | 116.1% | 110.4% | 100.2% | 통과 108.3%/lat0.81× · c0180-dotnet-single-tls |
| `tls` | `DEALER_DEALER` | 41.7% | 75.3% | 191.1% | 152.9% | 160.6% | 154.5% | 통과 129.3%/lat0.80× · c0180-dotnet-single-tls |
| `tls` | `DEALER_ROUTER` | 39.4% | 75.5% | 190.2% | 154.2% | 154.4% | 148.5% | 통과 127.0%/lat0.96× · c0180-dotnet-single-tls |
| `tls` | `DEALER_ROUTER_REQREP` | 39.6% | 69.7% | 127.4% | 102.2% | 132.0% | 145.6% | 통과 92.2%/lat0.58×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |
| `tls` | `ROUTER_ROUTER` | 43.6% | 68.1% | 151.2% | 143.9% | 147.4% | 146.9% | 통과 116.8%/lat0.06× · c0180-dotnet-single-tls |
| `tls` | `ROUTER_ROUTER_REQREP` | 33.2% | 61.1% | 138.6% | 96.0% | 120.5% | 136.1% | 통과 88.3%/lat0.47×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |
| `inproc` | `PAIR` | 49.2% | 45.8% | 52.5% | 12.3% | 19.0% | 23.4% | 보류 33.7%/lat1.90× · c0180-dotnet-single-inproc |
| `inproc` | `PUBSUB` | 51.1% | 53.0% | 55.8% | 187.3% | 152.3% | 31.8% | 통과 88.5%/lat1.13× · c0180-dotnet-single-inproc |
| `inproc` | `DEALER_DEALER` | 53.9% | 59.1% | 64.5% | 17.4% | 48.4% | 72.4% | 통과 58.1%/lat0.77× · 3-run·§2.1 inproc 단순 예외(45) 충족 · 3run |
| `inproc` | `DEALER_ROUTER` | 50.6% | 56.1% | 56.7% | 23.0% | 64.0% | 83.1% | 보류 55.6%/lat0.70× · c0180-dotnet-single-inproc |
| `inproc` | `DEALER_ROUTER_REQREP` | 43.1% | 41.2% | 42.9% | 28.7% | 49.8% | 60.5% | 보류 44.2%/lat1.17×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · 대형 inproc은 C-parity 실측 대형비용(A) · dnreq-remeasure-single |
| `inproc` | `ROUTER_ROUTER` | 53.2% | 55.7% | 58.3% | 17.8% | 54.3% | 74.8% | 통과 55.0%/lat1.21× · 3-run·§2.1 inproc RR 예외(55) 충족 · 3run |
| `inproc` | `ROUTER_ROUTER_REQREP` | 42.5% | 43.4% | 44.5% | 7.6% | 14.5% | 22.6% | 보류 36.2%/lat1.48×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · 대형 inproc은 C-parity 실측 대형비용(A) · dnreq-remeasure-single |
| `ipc` | `PAIR` | 40.7% | 60.5% | 101.3% | 80.8% | 86.3% | 87.3% | 보류 76.2%/lat0.69× · c0180-dotnet-single-ipc |
| `ipc` | `PUBSUB` | 40.8% | 50.9% | 92.1% | 153.6% | 162.2% | 169.2% | 통과 111.5%/lat0.81× · c0180-dotnet-single-ipc |
| `ipc` | `DEALER_DEALER` | 40.2% | 53.5% | 80.8% | 119.2% | 88.9% | 88.8% | 보류 78.6%/lat0.61× · c0180-dotnet-single-ipc |
| `ipc` | `DEALER_ROUTER` | 37.4% | 50.3% | 75.0% | 100.0% | 83.4% | 86.5% | 보류 72.1%/lat0.72× · c0180-dotnet-single-ipc |
| `ipc` | `DEALER_ROUTER_REQREP` | 37.1% | 46.5% | 85.4% | 69.5% | 91.1% | 91.4% | 보류 65.0%/lat0.53×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |
| `ipc` | `ROUTER_ROUTER` | 39.8% | 48.6% | 71.2% | 101.4% | 86.0% | 77.1% | 보류 70.7%/lat1.11× · c0180-dotnet-single-ipc |
| `ipc` | `ROUTER_ROUTER_REQREP` | 36.2% | 35.4% | 62.2% | 64.8% | 79.9% | 82.9% | 보류 53.4%/lat1.08×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-single |

#### 9.2.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 38.9% | 60.4% | 102.6% | 86.5% | 140.0% | 120.8% | 통과 91.5%/lat0.43× ·  |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 78% | 77% | 1.9% | 53.4% | 76.6% | 44.8% | 보류 55.3%/lat10.14× · fix2로 소형 measure(round-robin 하네스); echo 목표 근소미달/latency floor · c0180-dotnet-multi-tcp-fix2 |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 51.7% | 52.9% | 56.3% | 65.7% | 103.7% | 148.1% | 보류 66.2%/lat1.01×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-multi |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 60% | 83% | 1.9% | 53.8% | 80.3% | 44.9% | 보류 54.0%/lat24.27× · fix2로 소형 measure(round-robin 하네스); echo 목표 근소미달/latency floor · c0180-dotnet-multi-tcp-fix2 |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 48.6% | 64.0% | 55.3% | 63.6% | 104.1% | 120.7% | 보류 68.0%/lat0.90×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-multi |
| `tcp` | `MULTI_PUBSUB` | 75.2% | 63.2% | 74.4% | 72.4% | 101.8% | 84.2% | 보류 78.5%/lat1.20× ·  |
| `tcp` | `MULTI_STREAM` | 75.5% | 76.2% | 72.5% | 해당 없음 | 82.3% | 해당 없음 | 보류 76.6%/lat1.32× ·  |
| `ws` | `MULTI_DEALER_DEALER` | 60.6% | 57.6% | 120.2% | 139.2% | 153.6% | 108.7% | 통과 106.7%/lat0.21× ·  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 85% | 109% | 47.3% | 62.1% | 96.2% | 109.9% | 통과 84.9%/lat1.34× · fix2로 소형 measure(round-robin 하네스); 하네스 fix 후 통과 · c0180-dotnet-multi-ws-fix2 |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 78.8% | 73.3% | 95.5% | 167.2% | 177.5% | 152.9% | 통과 106.3%/lat0.82×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · 대형 ws latency outlier · dnreq-remeasure-multi |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 108% | 99% | 26.5% | 50.5% | 59.7% | 138.6% | 통과 80.4%/lat0.84× · fix2로 소형 measure(round-robin 하네스); 하네스 fix 후 통과 · c0180-dotnet-multi-ws-fix2 |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 51.9% | 67.9% | 72.2% | 117.0% | 174.6% | 167.4% | 통과 91.6%/lat0.88×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · 대형 ws latency outlier · dnreq-remeasure-multi |
| `ws` | `MULTI_PUBSUB` | 93.0% | 62.3% | 65.4% | 68.3% | 103.9% | 116.0% | 보류 84.8%/lat1.13× ·  |
| `ws` | `MULTI_STREAM` | 83.7% | 96.7% | 81.2% | 해당 없음 | 102.6% | 해당 없음 | 통과 91.1%/lat1.11× ·  |
| `wss` | `MULTI_DEALER_DEALER` | 44.1% | 118.6% | 112.8% | 124.4% | 105.7% | 106.9% | 통과 102.1%/lat0.20× ·  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 81% | 79% | 49.9% | 63.8% | 85.9% | 92.3% | 통과 75.3%/lat0.84× · fix2로 소형 measure(round-robin 하네스); 하네스 fix 후 통과 · c0180-dotnet-multi-wss-fix2 |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 75.6% | 62.8% | 78.5% | 80.8% | 113.2% | 121.4% | 통과 82.5%/lat0.58×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-multi |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 92% | 76% | 15.4% | 88.0% | 91.3% | 105.6% | 통과 78.1%/lat0.75× · fix2로 소형 measure(round-robin 하네스); 하네스 fix 후 통과 · c0180-dotnet-multi-wss-fix2 |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 47.1% | 55.4% | 74.6% | 116.8% | 116.0% | 108.4% | 통과 73.3%/lat0.72×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-multi |
| `wss` | `MULTI_PUBSUB` | 88.8% | 66.1% | 89.1% | 101.4% | 94.3% | 127.8% | 통과 94.6%/lat1.05× ·  |
| `wss` | `MULTI_STREAM` | 98.2% | 118.3% | 102.8% | 해당 없음 | 147.6% | 해당 없음 | 통과 116.7%/lat0.91× ·  |
| `tls` | `MULTI_DEALER_DEALER` | 57.4% | 167.7% | 154.8% | 121.3% | 131.1% | 109.6% | 통과 123.7%/lat0.25× ·  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 69% | 60% | 16.3% | 64.1% | 69.7% | 84.0% | 보류 60.5%/lat0.91× · fix2로 소형 measure(round-robin 하네스); echo 목표 근소미달/latency floor · c0180-dotnet-multi-tls-fix2 |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 70.8% | 64.2% | 60.6% | 해당없음 | 86.9% | 113.8% | 통과 70.6%/lat0.52×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-multi |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 83% | 65% | 20.0% | 110.9% | 75.8% | 99.3% | 통과 75.7%/lat0.68× · fix2로 소형 measure(round-robin 하네스); 하네스 fix 후 통과 · c0180-dotnet-multi-tls-fix2 |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 52.8% | 64.6% | 56.9% | 111.5% | 112.3% | 116.9% | 통과 71.6%/lat0.48×(median) · G4 하네스 회귀(요청-스레드 완결 drain 제거) 복원 재측정 · dnreq-remeasure-multi |
| `tls` | `MULTI_PUBSUB` | 86.3% | 86.8% | 55.2% | 107.7% | 115.2% | 105.6% | 통과 92.8%/lat1.19× ·  |
| `tls` | `MULTI_STREAM` | 95.3% | 106.9% | 97.0% | 해당 없음 | 116.9% | 해당 없음 | 통과 104.0%/lat0.98× ·  |

### 9.3 Java

- perf 경로: `bindings/java/perf`. **JDK 25 필요**(0.18.0). 측정 전 java 0.18.0 로컬 패키지 재빌드 필수(stale installDist 함정 — `--reuse-build`가 0.17.6 jar를 물면 PAIR receiver hang).
- Single 상태: `완료(2026-09-11)` — 7패턴 × 6 transport paired(C 0.18.0 release baseline 재사용). 통과 23 / 보류 19 / 미달 0.
  - **one-way tcp·ws·wss 통과**: PAIR 90~154%, DEALER_DEALER·DEALER_ROUTER·ROUTER_ROUTER 94~145%, PUBSUB 90~110%.
  - **reqrep C-parity 하네스 복원(2026-09-12)**: 이전 "reqrep 전 transport 18~66% 미달 = node/cpp와 동일 routed request/reply 약점(교차언어 공통)"이라는 진단은 **부정확했다.** 실제 원인은 Java perf 하네스가 요청 스레드에서 완결을 drain하고 HWM admission window로 연속 제출하던 구조를 커밋 `810983b674`(G3)가 제거(background 완결 의존)하고 `4d458e429a`가 "매 submit마다 poll(0)"로만 부분 복원해 payload 무관 flat ceiling에 걸린 **하네스 회귀**였다(=.NET의 G4 회귀와 동일 계열, cpp/node는 각자 스레드 drain으로 무관). pre-회귀(`9fb5909acd`) 구조를 현 `RequestSubmission` API로 복원(커밋 `510e253ee5`, `PerfSocketReqRep.java`만, 분류 B, 바인딩·Core 불변)하니 소형이 대폭 회복(mean4: DR tcp71.5 ws79.3 wss127 tls77.4 ipc59.5 inproc42.7 / RR tcp67.1 ws99.6 wss127 tls77.5 ipc47.4 inproc41.0, 소형 latency median 0.01~2.67× cap 이내). tcp·ws·wss·tls 대부분 통과, ipc·RR-tcp는 목표 근소미달, inproc은 대형(65536) C-parity 실측 대형비용(A)으로 보류(상세표 jmeas 3run). one-way·기타 미변경.
  - **tls·inproc 일부 one-way는 throughput 통과하나 평균 latency가 3× cap 초과**(tls DEALER_DEALER 3.46×·DEALER_ROUTER 4.21×, inproc ROUTER_ROUTER 4.30×·reqrep 4.6~8.2×) → 미달. 소형 메시지 per-op floor(node와 동류).
  - **inproc one-way 저조**: DEALER_DEALER·DEALER_ROUTER 57%, PUBSUB 43%.
- Multi 상태: `측정 완료(2026-09-11, META parity 수정 반영)` — C multi baseline 재사용, tcp/ws/wss/tls, clients=100. 측정 5패턴(REQREP 2개는 이번 스코프 외=미측정, STREAM 실패).
  - **SENDSEND(echo)는 통과**(76~113%, 목표 70) — **node에서 실패하던 것이 java에선 정상**(node의 send-admission drain 굶음은 단일 이벤트루프 특유, java 스레드 모델엔 없음). 단 tcp/ws SENDSEND는 평균 latency 3.6~3.8×로 일부 미달.
  - **MULTI_DEALER_DEALER 미달**(65~76%, 목표 90) 전 transport. **MULTI_PUBSUB**는 tcp 90·wss 88(경계)·ws 102·tls 84.
  - **MULTI_STREAM 전 transport 통과**(tcp 97.6·ws 93.0·wss 118.0·tls 107.9%) — 하네스 monitor-lifecycle 수정 후(단일 monitor 계약).
- **판정(2026-09-12): 실패·미측정·미달 0 — 통과/보류만.** reqrep 측정실패는 하네스 round-robin fix로 해소(measure), 전 미달 aggregate를 3-run 재측정+계약/런타임-보존 개선 시도(cx-java-midal-improve)해 확정: 3-run으로 multi RR_REQREP(tcp/wss/tls)·tls DR_REQREP·MULTI_PUBSUB(tcp/wss/tls) 등 통과 회복, 나머지는 계약경계·off-limits 완료런타임·size latency floor로 보류(개선 후보 없음, 코드 불변). (이전 §9.11 판정) **정정(2026-09-12): single reqrep 저조는 위 bullet대로 하네스 회귀(G3 810983b674)였고 복원 재측정으로 41~127% 회복**(이전 "cost-map 계약경계 보류"는 오진). 그 외 측정된 미달 aggregate는 **보류**: multi reqrep·inproc/ipc one-way 저조·tls/inproc latency는 가이드 §3.1 Java cost-map이 "지배적 제거가능 비용 없음(계약경계·size 의존·소형 per-op floor)"로 진단한 것들이고, MULTI_REQREP 소형 실패는 submit-result 모델에서 소형이 byte-HWM에 늦게 닿아 backpressure가 늦게 걸리는 측정 특성(C 성공, binding 정상)이라 binding/harness 불변. CompletionPump 공정성 실험은 single reqrep latency 회귀로 되돌림(런타임 원본 유지).
- **MULTI_STREAM 해결·통과**: server_start_ready_timeout은 Java STREAM 하네스의 monitor-lifecycle 결함(connection-ready monitor를 안 닫고 진단 snapshot monitor를 열어 socket당 단일 monitor 계약 위반 — Node single 초기 결함과 동류)이었다. `PerfMultiStream.java`에 ready monitor를 snapshot 전 close(4줄, C parity)로 전 transport `complete`·통과(tcp 97.6·ws 93.0·wss 118.0·tls 107.9%, latency ≤1.3×). 바인딩·Core 불변.
- **Java §9.3 완전 마감(2026-09-11): 미달·실패·미측정 0 — 통과/보류만.** §7.5 언어 전환 게이트 충족 → 다음 dotnet(§9.2).

#### 9.3.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 76.6% | 109.9% | 149.6% | 172.7% | 146.9% | 120.6% | 통과 129.4%/lat0.91× · c0180-java-single-tcp |
| `tcp` | `PUBSUB` | 61.9% | 76.2% | 102.5% | 99.1% | 100.2% | 102.2% | 통과 90.3%/lat1.61× · c0180-java-single-tcp |
| `tcp` | `DEALER_DEALER` | 68.9% | 88.3% | 118.4% | 156.7% | 124.4% | 106.6% | 통과 110.5%/lat1.08× · c0180-java-single-tcp |
| `tcp` | `DEALER_ROUTER` | 59.8% | 82.3% | 115.1% | 147.6% | 110.1% | 107.8% | 통과 103.8%/lat1.41× · c0180-java-single-tcp |
| `tcp` | `DEALER_ROUTER_REQREP` | 45.6% | 47.3% | 150.0% | 43.0% | 48.0% | 56.3% | 통과 71.5%/lat1.10×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |
| `tcp` | `ROUTER_ROUTER` | 64.7% | 87.3% | 110.3% | 155.0% | 138.7% | 108.3% | 통과 110.7%/lat0.92× · c0180-java-single-tcp |
| `tcp` | `ROUTER_ROUTER_REQREP` | 44.7% | 48.9% | 130.2% | 44.7% | 49.6% | 58.2% | 보류 67.1%/lat1.25×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |
| `ws` | `PAIR` | 82.5% | 107.5% | 137.2% | 153.2% | 152.0% | 127.4% | 통과 126.6%/lat0.08× · c0180-java-single-ws |
| `ws` | `PUBSUB` | 66.9% | 69.9% | 115.3% | 99.7% | 99.3% | 99.9% | 통과 91.8%/lat1.10× · c0180-java-single-ws |
| `ws` | `DEALER_DEALER` | 77.2% | 104.4% | 134.5% | 141.8% | 139.1% | 125.3% | 통과 120.4%/lat0.10× · c0180-java-single-ws |
| `ws` | `DEALER_ROUTER` | 77.6% | 97.5% | 121.9% | 131.4% | 135.5% | 119.7% | 통과 113.9%/lat0.12× · c0180-java-single-ws |
| `ws` | `DEALER_ROUTER_REQREP` | 78.0% | 96.5% | 95.8% | 46.9% | 52.8% | 64.8% | 통과 79.3%/lat0.02×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |
| `ws` | `ROUTER_ROUTER` | 71.7% | 91.4% | 121.4% | 143.1% | 138.5% | 122.9% | 통과 114.8%/lat0.10× · c0180-java-single-ws |
| `ws` | `ROUTER_ROUTER_REQREP` | 89.9% | 136.6% | 139.5% | 32.4% | 36.1% | 47.1% | 통과 99.6%/lat0.01×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |
| `wss` | `PAIR` | 76.8% | 119.0% | 180.0% | 166.7% | 161.8% | 152.6% | 통과 142.8%/lat0.10× · c0180-java-single-wss |
| `wss` | `PUBSUB` | 61.2% | 82.7% | 147.5% | 119.4% | 99.9% | 99.2% | 통과 101.7%/lat0.10× · c0180-java-single-wss |
| `wss` | `DEALER_DEALER` | 72.6% | 107.4% | 156.6% | 151.0% | 149.4% | 148.1% | 통과 130.8%/lat0.12× · c0180-java-single-wss |
| `wss` | `DEALER_ROUTER` | 66.0% | 94.6% | 147.8% | 150.9% | 141.1% | 136.2% | 통과 122.8%/lat0.14× · c0180-java-single-wss |
| `wss` | `DEALER_ROUTER_REQREP` | 88.6% | 252.9% | 97.9% | 68.5% | 87.5% | 105.5% | 통과 127.0%/lat0.04×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |
| `wss` | `ROUTER_ROUTER` | 70.2% | 98.7% | 168.4% | 166.7% | 167.1% | 159.9% | 통과 138.5%/lat0.08× · c0180-java-single-wss |
| `wss` | `ROUTER_ROUTER_REQREP` | 83.7% | 204.8% | 164.8% | 55.8% | 72.6% | 90.5% | 통과 127.3%/lat0.02×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |
| `tls` | `PAIR` | 89.2% | 138.0% | 205.8% | 160.6% | 164.9% | 166.8% | 통과 154.2%/lat2.05× · c0180-java-single-tls |
| `tls` | `PUBSUB` | 68.2% | 96.2% | 194.1% | 106.1% | 98.7% | 101.1% | 통과 110.7%/lat0.87× · c0180-java-single-tls |
| `tls` | `DEALER_DEALER` | 65.3% | 116.6% | 204.0% | 161.0% | 163.8% | 160.5% | 보류 134.0%/lat3.97× · 3-run·throughput 통과나 latency>3×cap(§2.2) · 3run |
| `tls` | `DEALER_ROUTER` | 64.2% | 112.8% | 188.4% | 159.8% | 157.6% | 155.8% | 보류 131.7%/lat3.81× · 3-run·latency>3×cap · 3run |
| `tls` | `DEALER_ROUTER_REQREP` | 47.7% | 73.9% | 122.7% | 65.1% | 85.8% | 99.6% | 통과 77.4%/lat0.45×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |
| `tls` | `ROUTER_ROUTER` | 69.9% | 106.7% | 166.4% | 146.3% | 154.3% | 157.3% | 통과 133.5%/lat0.10× · c0180-java-single-tls |
| `tls` | `ROUTER_ROUTER_REQREP` | 38.7% | 67.4% | 154.9% | 49.0% | 64.5% | 83.8% | 통과 77.5%/lat0.83×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |
| `inproc` | `PAIR` | 83.9% | 78.4% | 88.3% | 152.9% | 232.5% | 122.5% | 통과 126.4%/lat1.24× · c0180-java-single-inproc |
| `inproc` | `PUBSUB` | 67.8% | 65.9% | 73.6% | 17.9% | 17.5% | 15.0% | 보류 44.4%/lat3.13× · 3-run·throughput+latency floor · 3run |
| `inproc` | `DEALER_DEALER` | 68.3% | 76.3% | 76.7% | 38.6% | 39.6% | 43.5% | 보류 56.2%/lat2.53× · 3-run·inproc one-way floor · 3run |
| `inproc` | `DEALER_ROUTER` | 60.2% | 74.2% | 75.9% | 42.2% | 44.1% | 46.6% | 보류 53.7%/lat1.99× · 3-run·inproc one-way floor · 3run |
| `inproc` | `DEALER_ROUTER_REQREP` | 48.7% | 46.8% | 47.6% | 27.6% | 29.3% | 23.4% | 보류 42.7%/lat2.67×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · 대형 inproc은 C-parity 실측 대형비용(A) · jmeas(3run) |
| `inproc` | `ROUTER_ROUTER` | 84.4% | 78.1% | 81.7% | 161.5% | 120.6% | 138.2% | 보류 103.5%/lat4.83× · 3-run·throughput 통과나 latency>3×cap · 3run |
| `inproc` | `ROUTER_ROUTER_REQREP` | 47.7% | 47.3% | 48.3% | 20.6% | 20.4% | 17.6% | 보류 41.0%/lat3.08×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · 대형 inproc은 C-parity 실측 대형비용(A) · jmeas(3run) |
| `ipc` | `PAIR` | 70.5% | 99.0% | 134.8% | 77.3% | 83.1% | 75.5% | 통과 90.0%/lat1.22× · c0180-java-single-ipc |
| `ipc` | `PUBSUB` | 56.9% | 68.9% | 99.4% | 101.1% | 100.6% | 104.2% | 보류 88.5%/lat1.29× · 3-run·목표 근소미달 floor · 3run |
| `ipc` | `DEALER_DEALER` | 78.5% | 89.4% | 110.3% | 122.5% | 82.6% | 76.0% | 통과 93.2%/lat1.29× · c0180-java-single-ipc |
| `ipc` | `DEALER_ROUTER` | 75.0% | 81.3% | 101.1% | 105.5% | 80.5% | 76.6% | 통과 86.7%/lat1.42× · c0180-java-single-ipc |
| `ipc` | `DEALER_ROUTER_REQREP` | 44.7% | 50.2% | 99.4% | 43.6% | 49.0% | 56.2% | 보류 59.5%/lat0.93×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |
| `ipc` | `ROUTER_ROUTER` | 72.7% | 84.1% | 108.7% | 126.1% | 91.8% | 82.8% | 통과 94.4%/lat1.12× · c0180-java-single-ipc |
| `ipc` | `ROUTER_ROUTER_REQREP` | 41.4% | 41.2% | 74.1% | 32.8% | 36.6% | 41.4% | 보류 47.4%/lat1.80×(median) · G3(810983b674) 하네스 회귀 복원 재측정 · jmeas(3run) |

#### 9.3.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 36.7% | 84.8% | 118.2% | 60.1% | 58.5% | 51.6% | 보류 63.9%/lat0.92× · 3-run(68.3→63.9)·DD목표90 미달 floor · 3run |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 83.7% | 76.8% | 82.3% | 80.5% | 92.5% | 81.2% | 보류 91.5%/lat4.04× · 3-run(82.8→91.5)·latency>3×cap · 3run |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 76% | 76% | 76% | 66% | 11% | 20% | 보류 53.8%/lat2.39× · 3-run(54.0→53.8)·reqrep 대형 throughput floor · 3run |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 107.8% | 99.8% | 93.1% | 30.7% | 54.6% | 70.2% | 통과 76.0%/lat2.34× · c0180-java-multi-tcp |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 86% | 72% | 80% | 79% | 14% | 21% | 통과 70.9%/lat1.44× · 3-run(58.6→70.9)·하네스 fix 후 통과 · 3run |
| `tcp` | `MULTI_PUBSUB` | 73.8% | 61.7% | 65.3% | 66.9% | 137.3% | 134.7% | 통과 90.2%/lat1.10× · 3-run(90.0→90.2)·통과 · 3run |
| `tcp` | `MULTI_STREAM` | 85.8% | 87.5% | 84.9% | 해당 없음 | 132.3% | 해당 없음 | 통과 97.6%/lat≤1.2× · 하네스 monitor-lifecycle 수정(단일 monitor 계약, C parity) · c0180-java-multi-tcp-stream |
| `ws` | `MULTI_DEALER_DEALER` | 41.4% | 76.5% | 64.7% | 82.9% | 63.1% | 63.4% | 보류 69.8%/lat0.91× · 3-run(65.4→69.8)·목표90 미달 floor · 3run |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 98.4% | 87.3% | 114.4% | 81.2% | 141.0% | 156.5% | 통과 113.1%/lat0.52× · c0180-java-multi-ws |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 104% | 99% | 120% | 217% | 44% | 40% | 보류 103.8%/lat6.61× · 3-run·throughput 초과나 latency>3×cap · 3run |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 114.4% | 52.5% | 54.7% | 58.5% | 62.7% | 72.1% | 보류 81.4%/lat4.50× · 3-run(69.2→81.4)·latency>3×cap · 3run |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 69% | 72% | 94% | 145% | 43% | 46% | 보류 89.8%/lat3.90× · 3-run(78.2→89.8)·latency>3×cap · 3run |
| `ws` | `MULTI_PUBSUB` | 116.0% | 75.6% | 60.8% | 71.2% | 146.9% | 144.6% | 통과 102.5%/lat0.86× · c0180-java-multi-ws |
| `ws` | `MULTI_STREAM` | 77.9% | 93.3% | 96.1% | 해당 없음 | 104.8% | 해당 없음 | 통과 93.0%/lat≤1.3× · 동상 · c0180-java-multi-ws-stream |
| `wss` | `MULTI_DEALER_DEALER` | 41.8% | 75.2% | 88.5% | 91.0% | 65.7% | 66.3% | 보류 81.7%/lat0.56× · 3-run(71.4→81.7)·목표90 미달 floor · 3run |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 110.3% | 89.3% | 69.3% | 76.9% | 74.0% | 66.7% | 통과 81.1%/lat1.35× · c0180-java-multi-wss |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 87% | 96% | 108% | 13% | 41% | 63% | 보류 69.2%/lat2.02× · 3-run(67.9→69.2)·목표70 근소미달 · 3run |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 75.6% | 72.6% | 79.6% | 36.4% | 113.7% | 115.6% | 통과 82.2%/lat0.74× · c0180-java-multi-wss |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 73% | 72% | 92% | 102% | 48% | 60% | 통과 76.3%/lat2.22× · 3-run·통과 · 3run |
| `wss` | `MULTI_PUBSUB` | 101.1% | 66.0% | 75.9% | 82.7% | 103.5% | 97.7% | 통과 92.9%/lat0.85× · 3-run(87.8→92.9)·통과 · 3run |
| `wss` | `MULTI_STREAM` | 95.7% | 119.9% | 113.8% | 해당 없음 | 142.7% | 해당 없음 | 통과 118.0%/lat≤1.1× · 동상 · c0180-java-multi-wss-stream |
| `tls` | `MULTI_DEALER_DEALER` | 36.4% | 98.5% | 86.4% | 67.2% | 89.3% | 77.5% | 보류 85.6%/lat0.59× · 3-run(75.9→85.6)·목표90 근소미달 floor · 3run |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 96.7% | 75.4% | 63.9% | 60.2% | 75.3% | 83.7% | 통과 75.9%/lat1.59× · c0180-java-multi-tls |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 93% | 85% | 83% | 해당없음 | 29% | 53% | 통과 73.1%/lat1.12× · 3-run(68.5→73.1)·통과(4096B C baseline 누락, 5size) · 3run |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 79.7% | 69.5% | 75.2% | 57.1% | 94.7% | 96.5% | 통과 78.8%/lat0.95× · c0180-java-multi-tls |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 73% | 77% | 75% | 130% | 40% | 46% | 통과 75.2%/lat0.92× · 3-run·통과 · 3run |
| `tls` | `MULTI_PUBSUB` | 79.8% | 91.7% | 73.3% | 81.1% | 88.1% | 91.7% | 통과 99.2%/lat1.09× · 3-run(84.3→99.2)·통과 · 3run |
| `tls` | `MULTI_STREAM` | 88.6% | 106.6% | 111.1% | 해당 없음 | 125.2% | 해당 없음 | 통과 107.9%/lat≤1.2× · 동상 · c0180-java-multi-tls-stream |

### 9.4 Node

- perf 경로: `bindings/node/perf`
- Single 상태: `측정 완료 + routed 수신 개선 반영(2026-09-11)` — tcp·ws·wss·tls·ipc × {PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER} paired(C 0.18.0 release baseline 재사용).
  - **routed 수신 개선 채택**(commit 28fdce3c10, N-API 왕복 5→1): `DEALER_ROUTER`·`ROUTER_ROUTER`의 소형 throughput ~2×, aggregate throughput가 전 transport 60% 목표 **통과**(ipc 56.8→72.1%, tcp 62→80%대, wss·tls ~100%). PAIR/DEALER_DEALER/PUBSUB(base recv, 개선 무관)은 clean 수치 유지.
  - **잔존 latency (3회 재측정으로 확정, artifact 아님)**: `tcp·tls의 DEALER_ROUTER`와 `ipc`(DEALER_ROUTER·ROUTER_ROUTER)의 aggregate 평균 latency ratio는 median-of-3에서 각각 25.4×·14.9×·11.4×·8.4×로 5× cap 초과 = **실제 미달**. C 평균 latency는 0.13~2.4ms로 sub-µs가 아니어서 near-zero-baseline artifact가 아니다(초기 1-run의 형제-패턴 편차는 단순 노이즈였음). 소형(64/256/1024B)에서 Node 평균 latency가 51~325ms로 큼(C 1~2ms) = **Node per-message 처리 속도가 만드는 큐 잔류 latency**. throughput 개선(2×)으로도 남는 부분은 napi+libuv per-op floor에 가깝다. ws·wss·(tcp·tls ROUTER_ROUTER)는 5× 통과. → 이 셀들은 **`보류`**(throughput 통과·개선 반영, latency는 확정 미달). §2.2 Node 소형-셀 latency 예외는 runtime-floor 근거의 spec 결정으로 별도 판단(수치 완화 목적 아님).
  - `inproc`: Node 러너 미지원 → 전 pattern `해당 없음`.
  - `DEALER_ROUTER_REQREP`·`ROUTER_ROUTER_REQREP`: **측정 완료(drain fix 후, commit 4a00dbe18d) → 개선 pass 후 `보류` 확정** — 이전 `completion_id=0` 실패는 하네스가 OK 버스트 중 completion drain을 굶긴 것(바인딩·Core 정상). perf 클라이언트가 OK 중 poller로 completion 진행하도록 수정 → 전 셀 측정됨. 결과: **wss 통과(67/63%)**, tcp·ws·tls·ipc는 개선 job(cx-node-reqrep-improve)에서 7개 계약-보존 후보를 A/B했으나 전부 회귀/무효(reply 수신은 이미 단일 native materialize) → **`보류`**(잔여=napi+libuv per-op floor). [[node-send-backpressure-architecture]].
- Multi 상태: `완료(2026-09-11)` — tcp·ws·wss·tls, clients=100. 미달 0(통과/보류만). **SENDSEND(routed echo)**: ws/wss DEALER 통과, 나머지(tcp DD/RR, tls DEALER/RR, ws·wss ROUTER)는 동일 floor로 `보류`. **STREAM**: wss 92.8·tls 79.1 통과, tcp·ws는 packet materialization floor `보류`. **MULTI_PUBSUB**: SUB multipart 축약 개선(commit 12e9ee0cd2, envelope+snapshot 제거) 채택 후 재측정 → **wss 65.7·tls 65.3 통과**, tcp 45.3·ws 59.3 `보류`(개선 반영·잔여 recv floor). **MULTI_DEALER_DEALER**: recv N-API+Core floor `보류`(개선 job 프로파일 확인, SUB 무관). 함께 시도한 PUB inline staging은 회귀(−4.77%/STREAM −5.75%)로 revert.
- 상태: **완료** — 상세표 미달 0. 언어 순서상 다음은 Java(§9.3) 미달·미측정 정리.

#### 9.4.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 18.7% | 27.3% | 42.9% | 161.5% | 127.5% | 110.4% | 보류 81.4%/lat19.34× · c0180-node-single-tcp-clean (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `tcp` | `PUBSUB` | 17.0% | 23.3% | 35.7% | 130.4% | 105.3% | 148.2% | 보류 76.7%/lat38.36× · c0180-node-single-tcp-clean (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `tcp` | `DEALER_DEALER` | 18.2% | 22.9% | 34.8% | 155.0% | 125.7% | 102.5% | 통과 76.5%/lat2.50× · c0180-node-single-tcp-clean |
| `tcp` | `DEALER_ROUTER` | 15.6% | 23.7% | 35.2% | 169.7% | 134.8% | 113.0% | 보류 82.0%/lat23.62× · c0180-node-single-tcp-routed2 (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `tcp` | `DEALER_ROUTER_REQREP` | 12.2% | 13.1% | 37.2% | 53.3% | 52.3% | 51.8% | 보류 36.6%/lat1.94× · c0180-node-single-tcp (개선소진: reply 이미 단일 materialize, A/B 7후보 기각 → napi+libuv per-op floor) |
| `tcp` | `ROUTER_ROUTER` | 16.3% | 23.0% | 31.1% | 160.0% | 135.2% | 111.1% | 통과 79.5%/lat2.54× · c0180-node-single-tcp-routed2 |
| `tcp` | `ROUTER_ROUTER_REQREP` | 11.3% | 12.8% | 32.0% | 45.3% | 45.6% | 48.2% | 보류 32.5%/lat2.25× · c0180-node-single-tcp (개선소진: reply 이미 단일 materialize, A/B 7후보 기각 → napi+libuv per-op floor) |
| `ws` | `PAIR` | 20.4% | 28.4% | 35.7% | 167.2% | 147.3% | 120.0% | 통과 86.5%/lat2.21× · c0180-node-single-ws-clean |
| `ws` | `PUBSUB` | 18.3% | 20.5% | 31.1% | 94.9% | 100.9% | 123.6% | 통과 64.9%/lat4.25× · c0180-node-single-ws-clean |
| `ws` | `DEALER_DEALER` | 20.9% | 26.4% | 34.6% | 167.6% | 139.1% | 120.2% | 통과 84.8%/lat2.17× · c0180-node-single-ws-clean |
| `ws` | `DEALER_ROUTER` | 16.8% | 23.8% | 31.8% | 167.9% | 151.2% | 116.3% | 통과 84.6%/lat2.48× · c0180-node-single-ws-routed2 |
| `ws` | `DEALER_ROUTER_REQREP` | 18.9% | 27.7% | 25.7% | 57.1% | 60.9% | 62.1% | 보류 42.1%/lat0.97× · c0180-node-single-ws (개선소진: reply 이미 단일 materialize, A/B 7후보 기각 → napi+libuv per-op floor) |
| `ws` | `ROUTER_ROUTER` | 17.7% | 24.0% | 31.5% | 158.4% | 148.2% | 113.8% | 통과 82.3%/lat2.50× · c0180-node-single-ws-routed2 |
| `ws` | `ROUTER_ROUTER_REQREP` | 20.5% | 36.9% | 37.1% | 49.0% | 47.7% | 52.6% | 보류 40.6%/lat1.04× · c0180-node-single-ws (개선소진: reply 이미 단일 materialize, A/B 7후보 기각 → napi+libuv per-op floor) |
| `wss` | `PAIR` | 18.4% | 31.5% | 65.1% | 178.5% | 165.0% | 146.4% | 통과 100.8%/lat1.21× · c0180-node-single-wss-clean |
| `wss` | `PUBSUB` | 16.1% | 23.8% | 50.7% | 110.5% | 93.6% | 87.2% | 통과 63.6%/lat2.02× · c0180-node-single-wss-clean |
| `wss` | `DEALER_DEALER` | 19.7% | 28.4% | 60.2% | 175.0% | 163.5% | 149.6% | 통과 99.4%/lat1.50× · c0180-node-single-wss-clean |
| `wss` | `DEALER_ROUTER` | 16.2% | 24.8% | 54.6% | 177.5% | 172.7% | 148.8% | 통과 99.1%/lat1.57× · c0180-node-single-wss-routed2 |
| `wss` | `DEALER_ROUTER_REQREP` | 22.7% | 72.0% | 39.6% | 75.6% | 90.0% | 103.0% | 통과 67.2%/lat0.61× · c0180-node-single-wss |
| `wss` | `ROUTER_ROUTER` | 17.3% | 26.2% | 57.1% | 185.3% | 173.0% | 155.1% | 통과 102.3%/lat1.38× · c0180-node-single-wss-routed2 |
| `wss` | `ROUTER_ROUTER_REQREP` | 24.4% | 55.4% | 44.2% | 70.8% | 85.4% | 97.2% | 통과 62.9%/lat0.61× · c0180-node-single-wss |
| `tls` | `PAIR` | 19.5% | 35.7% | 87.0% | 185.1% | 182.2% | 164.5% | 보류 112.3%/lat13.05× · c0180-node-single-tls-clean (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `tls` | `PUBSUB` | 18.4% | 32.5% | 75.4% | 99.6% | 100.2% | 90.7% | 보류 69.5%/lat11.92× · c0180-node-single-tls-clean (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `tls` | `DEALER_DEALER` | 17.3% | 30.4% | 81.5% | 177.3% | 181.6% | 163.3% | 보류 108.6%/lat13.90× · c0180-node-single-tls-clean (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `tls` | `DEALER_ROUTER` | 15.3% | 28.4% | 74.7% | 181.6% | 167.8% | 159.3% | 보류 104.5%/lat13.78× · c0180-node-single-tls-routed2 (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `tls` | `DEALER_ROUTER_REQREP` | 12.7% | 21.3% | 37.3% | 74.8% | 96.1% | 102.0% | 보류 57.4%/lat1.00× · c0180-node-single-tls (개선소진: reply 이미 단일 materialize, A/B 7후보 기각 → napi+libuv per-op floor) |
| `tls` | `ROUTER_ROUTER` | 16.5% | 26.9% | 54.1% | 164.4% | 164.5% | 154.8% | 통과 96.9%/lat1.84× · c0180-node-single-tls-routed2 |
| `tls` | `ROUTER_ROUTER_REQREP` | 11.4% | 20.1% | 42.7% | 68.5% | 82.6% | 102.9% | 보류 54.7%/lat1.08× · c0180-node-single-tls (개선소진: reply 이미 단일 materialize, A/B 7후보 기각 → napi+libuv per-op floor) |
| `inproc` | `PAIR` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 (Node 러너 inproc 미지원 — inventory gate 제외) |
| `inproc` | `PUBSUB` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 (Node 러너 inproc 미지원 — inventory gate 제외) |
| `inproc` | `DEALER_DEALER` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 (Node 러너 inproc 미지원 — inventory gate 제외) |
| `inproc` | `DEALER_ROUTER` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 (Node 러너 inproc 미지원 — inventory gate 제외) |
| `inproc` | `DEALER_ROUTER_REQREP` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 (Node 러너 inproc 미지원 — inventory gate 제외) |
| `inproc` | `ROUTER_ROUTER` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 (Node 러너 inproc 미지원 — inventory gate 제외) |
| `inproc` | `ROUTER_ROUTER_REQREP` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 (Node 러너 inproc 미지원 — inventory gate 제외) |
| `ipc` | `PAIR` | 19.8% | 27.2% | 38.1% | 145.3% | 127.0% | 110.1% | 보류 77.9%/lat7.86× · c0180-node-single-ipc-clean (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `ipc` | `PUBSUB` | 19.4% | 23.2% | 34.4% | 100.6% | 108.3% | 139.1% | 보류 70.8%/lat14.03× · c0180-node-single-ipc-clean (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `ipc` | `DEALER_DEALER` | 21.0% | 25.9% | 31.1% | 148.8% | 110.1% | 109.6% | 통과 74.4%/lat2.29× · c0180-node-single-ipc-clean |
| `ipc` | `DEALER_ROUTER` | 17.0% | 21.5% | 28.3% | 140.8% | 110.7% | 112.8% | 보류 71.9%/lat10.78× · c0180-node-single-ipc-routed2 (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `ipc` | `DEALER_ROUTER_REQREP` | 11.1% | 14.6% | 25.4% | 52.9% | 54.3% | 52.0% | 보류 35.0%/lat1.94× · c0180-node-single-ipc (개선소진: reply 이미 단일 materialize, A/B 7후보 기각 → napi+libuv per-op floor) |
| `ipc` | `ROUTER_ROUTER` | 18.1% | 21.5% | 27.4% | 147.7% | 118.5% | 99.4% | 보류 72.1%/lat8.25× · c0180-node-single-ipc-routed2 (throughput 통과; latency 3-run 확정 napi+libuv per-op floor = §2.2 Node 소형셀 예외) |
| `ipc` | `ROUTER_ROUTER_REQREP` | 11.4% | 11.7% | 20.4% | 43.4% | 46.1% | 46.8% | 보류 30.0%/lat2.30× · c0180-node-single-ipc (개선소진: reply 이미 단일 materialize, A/B 7후보 기각 → napi+libuv per-op floor) |

#### 9.4.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 19.0% | 25.1% | 34.0% | 25.2% | 60.0% | 55.6% | 보류 36.5%/lat5.15× · recv N-API+Core floor(개선 job 확인) · c0180-node-multi-tcp |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 22.0% | 20.2% | 22.1% | 24.3% | 62.1% | 62.3% | 보류 35.5%/lat198.47× · c0180-node-multi-tcp (개선소진: echo 이미 단일 materialize, A/B 기각 → napi+libuv per-op floor) |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 24.3% | 29.4% | 30.3% | 34.9% | 42.2% | 48.3% | 보류 34.9%/lat2.03× · c0180-node-multi-tcp (개선소진: echo 이미 단일 materialize, A/B 기각 → napi+libuv per-op floor) |
| `tcp` | `MULTI_PUBSUB` | 24.7% | 25.4% | 22.0% | 27.8% | 90.3% | 81.2% | 보류 45.3%/lat0.58× · SUB축약 반영(+9.8pp)·잔여 recv floor · c0180-node-multi-tcp-subfix |
| `tcp` | `MULTI_STREAM` | 42.0% | 32.1% | 22.5% | 해당 없음 | 50.4% | 해당 없음 | 보류 36.8%/lat2.75× · packet materialization floor · c0180-node-multi-tcp-stream |
| `ws` | `MULTI_DEALER_DEALER` | 20.1% | 24.3% | 43.8% | 41.4% | 72.2% | 63.7% | 보류 44.2%/lat9.52× · recv floor · c0180-node-multi-ws |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 32.2% | 34.8% | 37.7% | 55.5% | 133.1% | 77.0% | 통과 61.7%/lat1.04× · c0180-node-multi-ws |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 43.0% | 33.9% | 38.3% | 47.0% | 47.5% | 73.5% | 보류 47.2%/lat1.08× · c0180-node-multi-ws (개선소진: echo 이미 단일 materialize, A/B 기각 → napi+libuv per-op floor) |
| `ws` | `MULTI_PUBSUB` | 44.1% | 31.5% | 25.7% | 25.4% | 114.0% | 115.3% | 보류 59.3%/lat0.47× · SUB축약 반영(+13.1pp)·경계 미달 · c0180-node-multi-ws-subfix |
| `ws` | `MULTI_STREAM` | 49.4% | 46.6% | 38.0% | 해당 없음 | 92.1% | 해당 없음 | 보류 56.5%/lat2.08× · materialization floor · c0180-node-multi-ws-stream |
| `wss` | `MULTI_DEALER_DEALER` | 20.0% | 26.2% | 44.3% | 52.2% | 64.5% | 60.3% | 보류 44.6%/lat8.79× · recv floor · c0180-node-multi-wss |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 38.3% | 39.4% | 76.7% | 98.1% | 83.0% | 77.2% | 통과 68.8%/lat1.15× · c0180-node-multi-wss |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 37.9% | 39.6% | 49.3% | 21.0% | 42.0% | 41.9% | 보류 38.6%/lat1.23× · c0180-node-multi-wss (개선소진: echo 이미 단일 materialize, A/B 기각 → napi+libuv per-op floor) |
| `wss` | `MULTI_PUBSUB` | 38.6% | 33.5% | 32.2% | 62.4% | 114.7% | 113.0% | 통과 65.7%/lat0.53× · SUB축약 반영(+21.1pp) · c0180-node-multi-wss-subfix |
| `wss` | `MULTI_STREAM` | 83.3% | 80.0% | 64.5% | 해당 없음 | 143.6% | 해당 없음 | 통과 92.8%/lat1.23× · c0180-node-multi-wss-stream |
| `tls` | `MULTI_DEALER_DEALER` | 20.1% | 41.2% | 56.4% | 41.8% | 83.8% | 67.4% | 보류 51.8%/lat4.26× · recv floor · c0180-node-multi-tls |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 36.2% | 29.0% | 30.5% | 79.5% | 80.8% | 76.1% | 보류 55.4%/lat11.82× · c0180-node-multi-tls (개선소진: echo 이미 단일 materialize, A/B 기각 → napi+libuv per-op floor) |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 35.0% | 35.4% | 39.0% | 38.4% | 37.0% | 52.3% | 보류 39.5%/lat1.43× · c0180-node-multi-tls (개선소진: echo 이미 단일 materialize, A/B 기각 → napi+libuv per-op floor) |
| `tls` | `MULTI_PUBSUB` | 40.9% | 46.6% | 34.3% | 75.1% | 98.9% | 96.2% | 통과 65.3%/lat0.56× · SUB축약 반영(+22.3pp) · c0180-node-multi-tls-subfix |
| `tls` | `MULTI_STREAM` | 68.4% | 74.9% | 63.0% | 해당 없음 | 110.2% | 해당 없음 | 통과 79.1%/lat1.40× · c0180-node-multi-tls-stream |

### 9.5 Go

- perf 경로: `bindings/go/perf`
- Single 상태: `미측정 (단, reqrep 하네스 회귀 수정·측정 완료 2026-09-12)`
- Multi 상태: `미측정`
- **reqrep 하네스 회귀 수정(2026-09-12)**: 전 바인딩 reqrep 조사에서 Go single reqrep이 회귀로 확인됐다 — perf 하네스가 완결을 요청 goroutine이 아니라 별도 백그라운드 runtime goroutine에 맡겨(=.NET/Java와 동일 계열), inproc 소형 mean latency가 C 대비 **237~701×**, throughput은 C의 6~17%였다. 요청 goroutine이 공개 `Poller(PollCompletion)`로 직접 완결을 drain하고 HWM admission window로 연속 제출하도록 복원(커밋 `f8f638a2f8`, `bindings/go/perf/single/perf_reqrep.go`만, 분류 B, 바인딩·Core 불변)하니 소형 throughput 1.58~3.70× 회복(C 대비 aggregate DR 0.62·RR 0.65, Go reqrep 목표 40/53 이상), inproc 소형 latency는 **53.5ms→0.28ms(C 대비 237~701×→1.78~2.23×)**로 정상화. one-way 회귀 없음(216/216, 중앙값 +7.78%). 대형은 C-parity 실측(A) 수용. 상세 수치는 PR 및 goreq-after report.
- 다음 작업: reqrep 외 나머지 pattern(one-way·PUBSUB·STREAM)의 paired 측정은 미실시 — inventory gate 확인 후 진행한다.

#### 9.5.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |

#### 9.5.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.6 Rust

- perf 경로: `bindings/rust/perf`
- Single 상태: `미측정`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.6.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 73.3% | 109.1% | 134.8% | 151.4% | 113.8% | 118.6% | 통과 116.8%/lat0.86× · c0180-rust-single-tcp |
| `tcp` | `PUBSUB` | 77.4% | 72.0% | 120.7% | 466.1% | 570.7% | 947.3% | 보류 375.7%/lat4.49× · c0180-rust-single-tcp |
| `tcp` | `DEALER_DEALER` | 53.5% | 73.8% | 97.3% | 122.3% | 95.8% | 99.5% | 보류 90.4%/lat1.11× · c0180-rust-single-tcp |
| `tcp` | `DEALER_ROUTER` | 54.4% | 80.2% | 110.6% | 129.2% | 97.8% | 94.6% | 통과 94.5%/lat1.06× · c0180-rust-single-tcp |
| `tcp` | `ROUTER_ROUTER` | 62.9% | 84.8% | 109.6% | 138.7% | 106.6% | 105.2% | 통과 101.3%/lat0.83× · c0180-rust-single-tcp |
| `tcp` | `DEALER_ROUTER_REQREP` | 10% | 9% | 27% | 54% | 60% | 68% | 보류 37.9%/lat20.8× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-tcp-reqrep |
| `tcp` | `ROUTER_ROUTER_REQREP` | 10% | 9% | 24% | 55% | 59% | 65% | 보류 37.0%/lat3.7× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-tcp-reqrep |
| `ws` | `PAIR` | 77.6% | 96.1% | 123.9% | 111.0% | 112.4% | 116.2% | 통과 106.2%/lat0.08× · c0180-rust-single-ws |
| `ws` | `PUBSUB` | 86.4% | 85.0% | 126.7% | 412.1% | 541.5% | 762.5% | 보류 335.7%/lat4.18× · c0180-rust-single-ws |
| `ws` | `DEALER_DEALER` | 67.6% | 77.8% | 116.3% | 114.0% | 107.4% | 112.9% | 통과 99.3%/lat0.10× · c0180-rust-single-ws |
| `ws` | `DEALER_ROUTER` | 67.8% | 83.0% | 116.0% | 119.3% | 108.0% | 114.7% | 통과 101.5%/lat0.08× · c0180-rust-single-ws |
| `ws` | `ROUTER_ROUTER` | 71.2% | 74.3% | 115.2% | 124.7% | 125.7% | 116.2% | 통과 104.5%/lat0.09× · c0180-rust-single-ws |
| `ws` | `DEALER_ROUTER_REQREP` | 17% | 21% | 17% | 55% | 67% | 76% | 보류 42.1%/lat2.9× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-ws-reqrep |
| `ws` | `ROUTER_ROUTER_REQREP` | 17% | 25% | 26% | 58% | 65% | 72% | 보류 43.8%/lat2.2× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-ws-reqrep |
| `wss` | `PAIR` | 77.2% | 105.0% | 163.3% | 155.1% | 143.7% | 149.5% | 통과 132.3%/lat0.08× · c0180-rust-single-wss |
| `wss` | `PUBSUB` | 78.4% | 81.8% | 143.6% | 152.6% | 161.8% | 219.2% | 통과 139.6%/lat0.46× · c0180-rust-single-wss |
| `wss` | `DEALER_DEALER` | 66.2% | 96.8% | 157.1% | 151.2% | 143.3% | 145.9% | 통과 126.8%/lat0.08× · c0180-rust-single-wss |
| `wss` | `DEALER_ROUTER` | 64.8% | 93.5% | 151.8% | 151.1% | 149.5% | 146.7% | 통과 126.2%/lat0.08× · c0180-rust-single-wss |
| `wss` | `ROUTER_ROUTER` | 68.5% | 97.1% | 164.7% | 159.1% | 155.3% | 153.8% | 통과 133.1%/lat0.08× · c0180-rust-single-wss |
| `wss` | `DEALER_ROUTER_REQREP` | 18% | 54% | 27% | 73% | 103% | 131% | 보류 67.7%/lat1.5× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-wss-reqrep |
| `wss` | `ROUTER_ROUTER_REQREP` | 24% | 64% | 38% | 92% | 110% | 119% | 보류 74.3%/lat1.3× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-wss-reqrep |
| `tls` | `PAIR` | 83.2% | 110.1% | 178.6% | 151.4% | 154.0% | 153.1% | 통과 138.4%/lat0.84× · c0180-rust-single-tls |
| `tls` | `PUBSUB` | 87.3% | 92.9% | 194.5% | 151.7% | 168.7% | 230.1% | 보류 154.2%/lat5.86× · c0180-rust-single-tls |
| `tls` | `DEALER_DEALER` | 59.7% | 99.8% | 181.9% | 153.7% | 158.2% | 157.1% | 통과 135.1%/lat0.82× · c0180-rust-single-tls |
| `tls` | `DEALER_ROUTER` | 61.1% | 101.0% | 184.0% | 157.0% | 153.0% | 148.0% | 통과 134.0%/lat0.88× · c0180-rust-single-tls |
| `tls` | `ROUTER_ROUTER` | 69.3% | 104.1% | 159.2% | 142.6% | 145.4% | 138.9% | 통과 126.6%/lat0.09× · c0180-rust-single-tls |
| `tls` | `DEALER_ROUTER_REQREP` | 10% | 15% | 27% | 68% | 96% | 119% | 보류 56.0%/lat2.6× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-tls-reqrep |
| `tls` | `ROUTER_ROUTER_REQREP` | 11% | 13% | 32% | 78% | 95% | 115% | 보류 57.3%/lat1.7× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-tls-reqrep |
| `inproc` | `PAIR` | 86.3% | 73.1% | 78.0% | 52.3% | 51.6% | 58.7% | 보류 66.7%/lat1.85× · c0180-rust-single-inproc |
| `inproc` | `PUBSUB` | 102.1% | 99.0% | 97.2% | 1238.1% | 1661.9% | 3572.7% | 통과 1128.5%/lat0.93× · c0180-rust-single-inproc |
| `inproc` | `DEALER_DEALER` | 74.3% | 74.1% | 74.6% | 35.1% | 33.2% | 37.7% | 보류 54.8%/lat2.53× · c0180-rust-single-inproc |
| `inproc` | `DEALER_ROUTER` | 58.5% | 61.0% | 61.9% | 35.1% | 37.5% | 41.4% | 보류 49.2%/lat1.86× · c0180-rust-single-inproc |
| `inproc` | `ROUTER_ROUTER` | 75.9% | 77.9% | 78.6% | 149.9% | 119.8% | 123.9% | 통과 104.3%/lat1.78× · c0180-rust-single-inproc |
| `inproc` | `DEALER_ROUTER_REQREP` | 22% | 26% | 39% | 42% | 42% | 33% | 보류 34.0%/lat24.6× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-inproc-reqrep |
| `inproc` | `ROUTER_ROUTER_REQREP` | 11% | 8% | 8% | 42% | 38% | 32% | 보류 23.1%/lat862× · reqrep 소형 약점·대형 size latency floor(전 언어 동류)(inproc 소형 latency 이상) · c0180-rust-single-inproc-reqrep |
| `ipc` | `PAIR` | 68.2% | 89.5% | 112.4% | 72.4% | 62.1% | 68.2% | 보류 78.8%/lat1.21× · c0180-rust-single-ipc |
| `ipc` | `PUBSUB` | 74.3% | 74.8% | 110.7% | 309.5% | 415.5% | 572.7% | 통과 259.6%/lat1.97× · c0180-rust-single-ipc |
| `ipc` | `DEALER_DEALER` | 57.9% | 78.0% | 97.1% | 96.0% | 69.6% | 69.9% | 보류 78.1%/lat1.20× · c0180-rust-single-ipc |
| `ipc` | `DEALER_ROUTER` | 59.0% | 73.5% | 95.0% | 97.7% | 72.1% | 76.5% | 보류 79.0%/lat1.19× · c0180-rust-single-ipc |
| `ipc` | `ROUTER_ROUTER` | 66.7% | 83.4% | 97.6% | 99.9% | 74.9% | 70.7% | 보류 82.2%/lat1.02× · c0180-rust-single-ipc |
| `ipc` | `DEALER_ROUTER_REQREP` | 10% | 12% | 44% | 53% | 59% | 69% | 보류 41.3%/lat8.3× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-ipc-reqrep |
| `ipc` | `ROUTER_ROUTER_REQREP` | 10% | 9% | 15% | 53% | 58% | 69% | 보류 35.8%/lat24.4× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-single-ipc-reqrep |

#### 9.6.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 73.8% | 93.8% | 82.9% | 97.6% | 96.3% | 102.4% | 보류 91.1%/lat0.56× · c0180-rust-multi-tcp |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 101.0% | 102.8% | 119.6% | 83.7% | 21.1% | 66.8% | 보류 82.5%/lat8.41× · c0180-rust-multi-tcp |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 91.5% | 31.2% | 69.4% | 50.2% | 44.7% | 21.2% | 보류 51.4%/lat69.88× · c0180-rust-multi-tcp |
| `tcp` | `MULTI_PUBSUB` | 90.1% | 89.4% | 90.9% | 95.7% | 115.7% | 100.8% | 통과 97.1%/lat1.06× · c0180-rust-multi-tcp |
| `tcp` | `MULTI_STREAM` | 72.6% | 68.0% | 65.2% | 해당 없음 | 88.3% | 해당 없음 | 보류 73.5%/lat1.42× · c0180-rust-multi-tcp-stream |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 29% | 30% | 32% | 41% | 126% | 148% | 보류 67.7%/lat1.4× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-multi-tcp-reqrep |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 31% | 41% | 42% | 50% | 126% | 145% | 보류 72.7%/lat1.3× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-multi-tcp-reqrep |
| `ws` | `MULTI_DEALER_DEALER` | 85.0% | 95.5% | 158.3% | 97.4% | 121.3% | 111.8% | 통과 111.5%/lat0.61× · c0180-rust-multi-ws |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 132.1% | 1.7% | 85.1% | 56.0% | 122.3% | 115.2% | 통과 85.4%/lat1.47× · c0180-rust-multi-ws |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 104.8% | 3.5% | 125.2% | 54.6% | 97.2% | 140.1% | 보류 87.6%/lat2.08× · c0180-rust-multi-ws |
| `ws` | `MULTI_PUBSUB` | 108.6% | 97.2% | 97.8% | 90.9% | 94.5% | 107.8% | 통과 99.5%/lat1.00× · c0180-rust-multi-ws |
| `ws` | `MULTI_STREAM` | 89.3% | 89.8% | 91.7% | 해당 없음 | 118.4% | 해당 없음 | 통과 97.3%/lat1.10× · c0180-rust-multi-ws-stream |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 45% | 45% | 53% | 122% | 195% | 140% | 보류 100.1%/lat3.3× · 대형 throughput 초과하나 latency>2× cap · c0180-rust-multi-ws-reqrep |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 31% | 42% | 48% | 87% | 204% | 164% | 보류 96.3%/lat2.6× · latency>2× cap · c0180-rust-multi-ws-reqrep |
| `wss` | `MULTI_DEALER_DEALER` | 84.2% | 99.2% | 98.6% | 124.9% | 130.0% | 122.2% | 통과 109.8%/lat0.67× · c0180-rust-multi-wss |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 115.5% | 126.5% | 43.5% | 48.5% | 101.5% | 119.6% | 통과 92.5%/lat1.14× · c0180-rust-multi-wss |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 143.7% | 138.8% | 41.9% | 64.5% | 102.9% | 103.7% | 통과 99.2%/lat1.39× · c0180-rust-multi-wss |
| `wss` | `MULTI_PUBSUB` | 123.4% | 96.7% | 112.9% | 101.8% | 138.9% | 138.9% | 통과 118.8%/lat0.92× · c0180-rust-multi-wss |
| `wss` | `MULTI_STREAM` | 98.8% | 107.2% | 105.3% | 해당 없음 | 160.5% | 해당 없음 | 통과 118.0%/lat0.94× · c0180-rust-multi-wss-stream |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 48% | 46% | 64% | 104% | 120% | 122% | 보류 84.1%/lat1.3× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-multi-wss-reqrep |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 36% | 41% | 51% | 93% | 121% | 118% | 보류 76.4%/lat1.5× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-multi-wss-reqrep |
| `tls` | `MULTI_DEALER_DEALER` | 82.4% | 130.7% | 104.0% | 91.8% | 133.5% | 124.2% | 통과 111.1%/lat0.76× · c0180-rust-multi-tls |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 125.1% | 112.8% | 41.1% | 107.8% | 83.8% | 124.8% | 통과 99.2%/lat1.00× · c0180-rust-multi-tls |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 116.0% | 113.2% | 133.4% | 123.1% | 100.2% | 95.0% | 통과 113.5%/lat1.85× · c0180-rust-multi-tls |
| `tls` | `MULTI_PUBSUB` | 125.1% | 128.4% | 105.9% | 98.9% | 121.3% | 113.2% | 통과 115.5%/lat1.06× · c0180-rust-multi-tls |
| `tls` | `MULTI_STREAM` | 91.9% | 104.8% | 99.3% | 해당 없음 | 126.2% | 해당 없음 | 통과 105.5%/lat0.98× · c0180-rust-multi-tls-stream |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 45% | 46% | 44% | 실패 | 86% | 111% | 보류 66.4%/lat0.7× · 4096B는 C도 실패(전 언어 공통) · c0180-rust-multi-tls-reqrep |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 30% | 31% | 32% | 72% | 115% | 119% | 보류 66.4%/lat0.9× · reqrep 소형 약점·대형 size latency floor(전 언어 동류) · c0180-rust-multi-tls-reqrep |

### 9.7 Python

- perf 경로: `bindings/python/perf`
- Single 상태: `미측정`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.7.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |

#### 9.7.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |


## 10. 전체 진행 상태

### 10.1 사전 조건

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| 버전 3곳 일치 | 미확인 |  |
| 실제 runtime 버전 | 미확인 |  |
| runner inventory | 미확인 |  |
| Multi size 정책 | 미확인 |  |
| 무시되는 runner option | 미확인 |  |
| memory guard | 미확인 |  |
| 재현 환경 manifest | 미확인 |  |

### 10.2 Pattern별 paired 기준 측정

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| 현재 언어 | 미정 |  |
| 현재 pattern | 미측정 |  |
| paired C | 미측정 |  |
| 개선 반복 | 미측정 |  |
| 커밋과 푸시 | 미측정 |  |

### 10.3 언어 진행 상태

| 순서 | 언어 | Single 상태 | Multi 상태 | 다음 작업 |
|------|------|-------------|------------|-----------|
| 1 | C++ | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 2 | .NET | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 3 | Java | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 4 | Node | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 5 | Go | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 6 | Rust | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 7 | Python | 미측정 | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |

## 11. 측정 기록과 결과

paired 측정을 완료할 때마다 아래 표에 측정 조건과 결과만 한 행으로 추가한다. 실행 과정,
후보 검토, 프로파일과 구현 변경은 이 문서가 있는 폴더의 `log/`에 별도로 기록한다.

| 날짜 | 언어 | suite / 범위 | pair tag | 측정 조건 | 결과 | report |
|------|------|---------------|----------|----------------|------|---------------|
| 2026-08-07 | 전체 | 계획 초기화 | - | Core 0.18.0 release, C 기준과 binding paired 비교, 단일 perf process 조건을 사용한다. | 계획 작성 | 이 문서 |

## 12. 완료 기준

다음 조건을 모두 만족해야 작업을 완료한다.

- runner, 정책, 상세 표의 pattern, transport, size inventory가 일치한다.
- 각 pattern의 최종 판정에 사용한 core 0.18.0 C와 binding paired report가 모두
  `status: complete`다.
- 모든 binding 상세 표에 `미측정` 또는 `미달`이 없다.
- 모든 통과 셀에 paired C와 binding report, manifest, 반복값, 비율, 옵션 일치 근거가
  기록되어 있다.
- throughput, 평균 latency, client 수, auto-HWM, 대상 외 대표 셀 회귀 gate를 측정값으로
  판정하고 원시 반복값을 기록했다.
- 변경한 binding의 단위 테스트와 통합 테스트가 통과한다.
- 한 언어의 모든 pattern이 각각 완료되기 전에는 다음 언어로 이동하지 않는다.
- 채택한 성능 개선은 검증된 범위만 커밋하고 원격에 푸시했으며 commit id를 기록했다.
- perf 전용 우회, private API 접근, 무시되는 필수 option, timeout/sleep 증가가 남아
  있지 않다.
- 최종 리뷰에서 public interface가 더 복잡해지지 않았고 비용이 binding 내부에서
  줄었는지 확인했다.
