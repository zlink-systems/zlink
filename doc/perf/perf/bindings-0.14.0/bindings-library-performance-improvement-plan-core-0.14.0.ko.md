# core 0.14.0 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-08-28
>
> 작업 브랜치: `core-0.14.0-bindings-performance`
>
> 이 브랜치에서 작업하도록 승인되었으며, 측정과 문서 변경은 고정한 WSL/Linux 작업영역에서 진행한다.
>
> 이 문서는 core 0.14.0을 기준으로 bindings 라이브러리 성능 개선을 처음부터
> 진행하기 위한 실행 문서다. 이전 계획 문서의 측정값과 완료 판정은 가져오지 않는다.
> 새 C 기준 결과와 각 binding의 새 결과만 이 문서에 기록한다. 이 계획서에는 측정 대상,
> 측정 조건, report 경로, 비교값과 판정만 남긴다. 실행 명령, 후보 검토, 프로파일과 같은
> 과정 설명은 이 문서가 있는 폴더의 `log/`에 기록한다.

## 1. 기준 버전과 시작 상태

이번 작업의 core 기준 버전은 0.14.0이다. 측정 전에 다음 세 파일의 버전이 모두 같은지
확인한다.

- `VERSION`: `LIBZLINK_VERSION=0.14.0`
- `core/CMakeLists.txt`: `project(zlink VERSION 0.14.0 ...)`
- `core/include/zlink.h`: major, minor, patch values matching 0.14.0

`bindings/tools/local_core_runtime.sh`는 `VERSION`의 값을 이용해 GitHub의
`core/v0.14.0` release asset을 기존 release 절차로 가져오고 versioned
runtime 경로를 선택한다. 따라서 파일 이름이나 `Perf runtime libzlink: ...` 경로만
보고 판정하지 않는다. runner 또는 binding의 public version API와
`share/zlink/core-package-provenance.json`이 보고한 실제 runtime 버전도
0.14.0인지 확인한다.

측정을 시작할 때는 Core source를 다시 build하지 않는다. 모든 perf runner는 기본적으로
LOCAL Core build를 선택한다. 공식 측정에서는 runner에 `--core-version 0.14.0`를
전달해 검증된 release runtime을 선택하며, 이 option이 release prefix와 package provenance를
resolve하고 verify한다. `ZLINK_CORE_SOURCE`를 명시적으로 export한 경우에는 그 값이 runner의
기본 선택보다 우선한다. `core/build`와 현재 source 변경은 측정 runtime을 구성하지 않는다.
다른 버전의 local package나 오래된 runtime을 사용한 결과도 이 문서의 기준값으로 사용하지
않는다.

모든 성능 셀은 `미측정`에서 시작한다. 상세 표에는 현재 binding runner에 실제로 등록된
pattern만 포함한다. 공식 C runner에만 있고 binding runner에 없는 pattern은 이 계획의
측정 대상에서 제외한다. 이전 문서와 이전 report는 병목 후보를 찾는 참고 자료로만 사용하며,
core 0.14.0의 통과 비율이나 완료 근거로 사용하지 않는다.

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

비교 기준은 같은 core 0.14.0 runtime으로 실행한 `bindings/c/perf` 결과다. 같은 suite,
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
96.3%였고 routed one-way와 multi routed echo도 비슷했다. 다만 현재 core 0.14.0의
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
근거로 쓰지 않으며 Node와 같은 60% 중앙값 목표에서 시작한다. 이후 현재 core 0.14.0의
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

목표 경계 셀과 secure transport는 5회 반복 결과로 판정한다. 최적화 전후를 비교할 때
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

core 0.14.0의 현재 multi runner 기본값을 따른다. 이전 표의 256 KiB는 제거하고
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
| 탐색 | 기본 duration, 1회 | 병목 후보 선별 |
| 후보 판정 | 기본 duration, 3회 | before/after와 C 대비 비율 판정 |
| 최종·경계 판정 | 기본 duration, 5회, CPU pin 없음 | 필요할 때 반복값을 추가 기록하는 측정 근거 |

반복 횟수는 perf 정책의 실행 조건을 따른다. `runs=1`이면 해당 측정값을 사용하고,
`runs>1`이면 metric별 median을 대표값으로 사용한다. 원시 반복값은 측정 기록에 남기며
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
2. GitHub `core/v0.14.0` release asset과 package provenance를 준비하고 재현
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

## 7-1. 작업 지시와 운영 규칙

> 2026-08-27 사용자 지시로 확정한 운영 규칙이다. core 0.13.2 작업에서 정립했고
> 0.14.0 작업에도 그대로 적용한다. 7절의 실행 절차와 함께 지킨다.
> 세션이 바뀌어도 이 규칙을 그대로 적용한다.

### 7-1-1. 목표의 정의

- **목표는 perf 수치를 올리는 것이 아니라 bindings 라이브러리 자체를 최적화하는 것이다.**
  성능 수치는 회귀 여부를 확인하는 근거일 뿐 목표가 아니다.
- 제거 대상: 불필요한 heap 할당·재할당, 불필요한 복사, 불필요한 경합·동기화,
  불필요한 상태·분기·간접 dispatch·native boundary 왕복.
- POSDDD 구조 리팩토링을 함께 진행한다.
  **성능 개선이 없어도 구조가 개선되었다면 채택할 수 있다.**
  단 성능 회귀, 새 복잡성, contract 위험이 있으면 채택하지 않는다.
- **성능 개선 작업의 대상 언어는 cpp, dotnet, java, node 4개다.**

### 7-1-2. perf harness 수정 기준

- binding perf runner를 C perf runner와 대조해, **binding 쪽에만 있는 불필요한 작업**
  (여분의 복사·할당, 불필요한 동기화·대기, C에는 없는 추가 연산, 다른 warmup/측정 경계)을
  찾으면 근거를 남기고 제거·정정한다. 두 runner가 같은 의미의 작업을 측정하게 맞추는 것이 기준이다.
- 금지되는 것은 **수치를 좋게 만들기 위한 튜닝**뿐이다(측정 대상 축소, 조건 완화,
  C에는 없는 유리한 최적화 추가).
- 변경할 때마다 "C runner는 무엇을 하고 binding runner는 무엇을 더/다르게 하고 있었는지"를 기록한다.
- C runner 쪽 결함이면 고치지 말고 보고한다. 기준이 흔들리면 안 된다.

### 7-1-3. 동기화 제거 기준

- **core는 thread safe다.** binding이 core가 이미 보장하는 것을 다시 동기화하고 있으면 제거 대상이다.
- 제거·축소할 때는 **어떤 core spec 조항이 그것을 대신 보장하는지** 원문 인용으로 근거를 남긴다.
  근거를 찾지 못하면 제거하지 말고 후보로만 보고한다.
- binding 자신의 계약(정확히 한 번 완료, callback context, cancellation 순서,
  close 중 in-flight 보호, multipart 파트 순서)이 실제로 요구하는 보호는 남긴다.
- 동기화를 줄인 변경은 **경합 stress 검증이 필수**다. 다중 thread 송신·close 혼합을 최소 수만 회,
  가능하면 TSAN 또는 ASAN/UBSAN 빌드로 실행한다.

### 7-1-4. spec 수정 권한

- **core spec(`core/doc/spec/`)은 수정하지 않는다.** 변경 의견만 남긴다(사용자 승인 시에만 반영).
- **bindings spec(`bindings/doc/spec/`)은 감독관이 수정한다. sub-agent는 수정하지 않는다.**
- spec과 구현이 다르면 **구현을 spec에 맞춘다.**
  단 **구현이 옳고 spec이 틀린 경우가 있다.** 그때는 spec을 고친다(판단은 감독관).
- core spec이 상위 기준이다. bindings 라이브러리는 core spec의 의미를 그대로 반영한다.
  언어적 특성(예외 vs 반환값, async 표현 방식)은 고려하되 **의미는 모든 언어 binding이 동일해야 한다.**
- **public contract의 public interface(타입·시그니처·이름·소유권 규칙·반환 계약)는 절대 변경하지 않는다.**
  변경이 필요하다고 판단되면 구현하지 말고 의견만 남긴다.
- **계약을 완화해 목표를 맞추지 않는다.** test를 통과시키려고 assert를 완화하거나
  timeout을 늘려 회피하지 않는다.
- spec 변경 의견과 public interface 변경 의견은
  [`spec-and-interface-change-proposals.ko.md`](../bindings-0.13.2/spec-and-interface-change-proposals.ko.md)에 누적한다.

### 7-1-5. spec 변경에서 비롯된 코드 수정

- spec(특히 공통 `bindings/doc/spec/README.{ko,en}.md`) 변경으로 계약이 바뀌면,
  그 계약을 구현한 **모든 bindings를 함께 수정한다.**
  성능 측정 대상이 4개 언어라도 계약 변경은 해당 계약을 구현한 모든 언어에 적용한다.
- 언어별 spec에 같은 서술이 반복되어 있으면 함께 갱신한다. 한 언어만 고쳐 의미가 갈리게 두지 않는다.
- 각 언어의 contract test로 개별 검증하고 언어별 결과 수치를 따로 보고한다.
- 그 계약을 구현하지 않는 언어는 "해당 없음"을 근거와 함께 명시한다.

### 7-1-6. core 버그 처리

- 원인이 core 구현의 버그이면 **core에 회귀 test를 먼저 작성하고 버그를 수정한다.**
  binding에서 우회하거나 감싸서 덮지 않는다.
- 회귀 test는 그 버그를 재현해야 한다. **수정 전 실패, 수정 후 통과**를 확인하고 수치를 보고한다.
- 구현이 core spec을 위반한 경우는 버그이므로 구현을 고친다(core spec은 그대로 둔다).

### 7-1-7. 작업 요청 단위

- **1 run = 표의 한 줄(transport + pattern)** 이 기본 단위다.
- 한 row의 흐름:
  1. 해당 transport+pattern만 C -> binding paired 측정, size별 비율·aggregate·latency 중앙값 산출
  2. 목표 충족이면 `통과(비율%)`로 표 갱신 후 종료
  3. 미달이면 그 줄에서 개선(perf runner 대조 -> binding 불필요 비용 제거 -> POSDDD) 후 재측정
  4. 산출물: log 파일 1개 + 계획 문서 표 1줄 + xlsx 원시값
- **빌드는 매 run 반복하지 않는다.** 언어별 첫 run에서 Core·C runner·binding runner를 빌드하고,
  이후 row run은 기존 빌드를 재사용하되 binding 소스가 바뀌면 그 binding만 재빌드한다.
- **cross-cutting 후보**(여러 row·여러 언어에 공통 영향)는 row 안에서 구현하지 말고 **보고만** 한다.
  감독관이 별도 run으로 분리하고, 반영 후 영향받은 row를 재측정한다.
  한 row에서 공통 경로를 바꾸면 앞서 통과 처리한 row들의 근거가 무효가 되기 때문이다.

### 7-1-8. 개선 pass 횟수

| row 상태 | pass 횟수 | 결과 처리 |
|---|---:|---|
| aggregate 통과 + 일부 size만 미달 | 1회 | 남은 미달 size는 outlier로 기록, `통과(비율%)` 확정 |
| aggregate 미달 | 최대 3회 | 3회 내 도달 시 `통과`, 미도달 시 `보류(미달 비율%)` 후 즉시 다음 row |

- 각 pass는 서로 다른 후보를 구현·측정한다. cross-cutting 후보는 pass로 세지 않는다.
- **구조 개선 포인트도 성능 개선 포인트도 더 없으면 지체 없이 `보류` 확정하고 즉시 다음 항목으로 이동한다.**
  같은 후보를 각도만 바꿔 재시도하거나 이미 no-go로 판정한 것을 다시 파지 않는다.
- `보류` 확정은 감독관이 하며 **사용자에게 되묻지 않는다.**
  확정 전에 남길 것은 3절 규정 그대로다: 유효한 paired 재측정, 구현·측정된 후보와 결과,
  후보 소진 근거, contract 회귀 결과.

### 7-1-9. 측정 격리 (최우선)

- **성능 측정은 언제나 단독 실행한다.** 측정 중에는 다른 agent의 build, test, 측정을 돌리지 않는다.
  동시 실행은 간섭으로 수치를 무효화한다.
- 병렬화는 **측정이 아닌 작업**에만 적용한다: 코드 변경, 코드 분석, contract test 작성, 구조 리팩토링.
- 병렬 작업이 끝난 뒤 측정은 **한 번에 하나씩 순차로** 수행한다.
- 다른 작업이 도는 중에 나온 측정값은 **폐기**하고 단독 조건에서 다시 측정한다.

### 7-1-10. 실행 주체와 model 배분

- 사용자와 대화하는 세션은 **감독·리뷰만 한다.** 실제 작업은 codex sub-agent가 수행한다.
- 언어별 코드 작업은 **언어별 agent 하나씩 병렬**로 진행할 수 있다.
  이때 각 agent는 **자기 언어 디렉터리만 수정**하고 core·spec·다른 언어는 읽기 전용으로 둔다.

| 작업 유형 | model |
|---|---|
| 계약 판정, 회귀 원인 규명, 최적화·구조 리팩토링 설계 | `gpt-5.6-sol` |
| row 측정·기록, 빌드, 표/log/xlsx 갱신 | `gpt-5.6-terra` |
| 단순 기계적 작업 | `gpt-5.6-luna` |

- 실행 중인 agent는 model을 바꾸려고 중단·재투입하지 않는다(재작업 비용이 더 크다).
- sub-agent 지시서에는 항상 다음을 포함한다: branch 보호(전환·reset·restore·강제 checkout 금지),
  기존 미커밋 변경 보존, commit·push 금지, 저장소 루트에 임시 파일 생성 금지,
  범위 밖 금지 항목 명시, 정지 조건, 실행할 검증과 "수치를 그대로 보고" 요구.

### 7-1-11. 진행 감시

- sub-agent 진행 여부는 **로그 바이트 증가**와 **프로세스 생존**으로 판단한다.
  정상 종료 시 로그 끝에 최종 보고와 토큰 사용량 마커가 남는다.
- 로그가 15~20분간 증가하지 않으면 정체로 보고 원인을 확인한다.
  최종 보고 없이 프로세스만 사라지면 비정상 종료다.
- 장시간 test가 걸려 있으면(예: JVM이 `futex_wait_queue` 정지) 그 프로세스를 확인한다.
  binding 결함일 수 있다.

### 7-1-12. 중단 없이 진행할 것

- **사용자에게 되묻지 않는다.** public interface는 변경하지 않고, core spec은 변경하지 않고,
  계약은 완화하지 않으며, 목표 미달이 남으면 `보류`로 확정하고 다음으로 진행한다.
- 변경 의견은 제안 문서에 누적해 작업 완료 후 전달한다.

## 8. 판정과 기록 방법

상태 값은 다음과 같이 사용한다.

- `미측정`: 같은 조건의 core 0.14.0 C 결과와 binding 결과를 아직 비교하지 않았다.
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

### 9.1 C++

- perf 경로: `bindings/cpp/perf`
- Multi 상태: `TCP/WSS 1024B 최종 r5 측정 완료, DD 개선 후보 r3 확인·최종 r5 대기`
- paired report: C `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260830_022104_final-0146-c-baseline-r5.txt`, C++ `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260830_040436_final-0146-cpp-optimized-r5.txt`
- 조건: Core 0.14.6(`22e608ccdc`), Release/LTO, duration 5초, runs 5, clients 100, I/O threads 4, TCP/WSS, 1024B, 두 report 모두 `status: complete`.
- async send completion은 Core가 nonzero operation id를 반환한 경우에만 self-reference를 유지한다. callback이 없는 즉시 완료 경로에서 상태 mutex 왕복을 생략한 r3는 직전 후보보다 TCP 8.6%, WSS 4.9% 개선됐다. `perf_cpp_multi_linux_20260830_090317_cpp-retain-pending-only-r3.txt`

#### 9.1.1 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 보류(71.10%; 후보 r3) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 578.3/813.4 Kmsg/s, 평균 지연 0.254/0.939 ms(0.270x). nonzero operation에만 completion self-reference를 유지하는 후보이며 최종 r5 대기. `perf_cpp_multi_linux_20260830_090317_cpp-retain-pending-only-r3.txt` |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 통과(85.18%) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 176.8/207.5 Kops/s, 평균 지연 1.143/0.838 ms(1.364x). |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 보류(107.98%; 지연 2.507x) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 88.8/82.2 Kops/s, 평균 지연 1.449/0.578 ms. 공개 async queue/teardown 개선 후 지연 상한 미달. |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 통과(88.68%) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 141.2/159.3 Kops/s, 평균 지연 1.460/0.885 ms(1.650x). |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 보류(122.79%; 지연 2.380x) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 112.4/91.5 Kops/s, 평균 지연 1.159/0.487 ms. 공개 async queue/teardown 개선 후 지연 상한 미달. |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 보류(78.60%) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 600.8/764.4 Kmsg/s, 평균 지연 1315.997/1042.517 ms(1.262x). |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 보류(17.66%) | 해당 없음 | 미측정 | 해당 없음 | 처리량 C++/C 21.4/121.3 Kops/s, 평균 지연 1070.086/626.407 ms(1.708x). STREAM 전용 구조 검토와 no-go A/B 완료. |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 보류(76.81%; 후보 r3) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 515.1/670.6 Kmsg/s, 평균 지연 2.020/3.146 ms(0.642x). nonzero operation에만 completion self-reference를 유지하는 후보이며 최종 r5 대기. `perf_cpp_multi_linux_20260830_090317_cpp-retain-pending-only-r3.txt` |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 통과(90.52%) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 129.4/143.0 Kops/s, 평균 지연 5.437/4.586 ms(1.186x). |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 보류(104.71%; 지연 3.221x) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 90.0/85.9 Kops/s, 평균 지연 5.678/1.763 ms. 공개 async queue/teardown 개선 후 지연 상한 미달. |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 보류(82.20%) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 136.1/165.6 Kops/s, 평균 지연 5.130/3.536 ms(1.451x). 자체 개선 후 중앙값 목표 85% 미달, 잔여 안전 후보 없음. |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 보류(86.75%; 지연 8.549x) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 75.1/86.6 Kops/s, 평균 지연 14.499/1.696 ms. 공개 async queue/teardown 개선 후 지연 상한 미달. |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 보류(87.96%) | 미측정 | 미측정 | 미측정 | 처리량 C++/C 416.1/473.1 Kmsg/s, 평균 지연 1028.827/922.309 ms(1.115x). 중앙값 목표 95% 미달, 잔여 안전 후보 없음. |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 보류(101.50%; 지연 3.166x) | 해당 없음 | 미측정 | 해당 없음 | 처리량 C++/C 152.5/150.2 Kops/s, 평균 지연 34.044/10.754 ms. STREAM 전용 구조 검토와 no-go A/B 완료. |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.2 .NET

- perf 경로: `bindings/dotnet/perf`
- Multi 상태: `TCP/WSS 1024B 최종 r5 및 현재 tree 후보 r3 완료, 목표 미달 hot path 추가 검토 중`
- paired report: C `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260830_022104_final-0146-c-baseline-r5.txt`, .NET `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260830_051028_final-0146-dotnet-r5.txt`
- 조건: Core 0.14.6(`22e608ccdc`), Release/LTO, duration 5초, runs 5, clients 100, I/O threads 4, TCP/WSS, 1024B, 70/70 성공, 두 report 모두 `status: complete`.
- 측정 중 발견한 REQ/REP 종료 직후 stale route(`NotConnected`) 경합은 정상 teardown으로 처리하도록 수정했고 최종 20개 REQ/REP case에서 재발하지 않았다. direct 2-part native submit 후보는 별도 3회 A/B에서 SENDSEND TCP -14~-16%, REQ/REP -8~-41% 회귀해 제거했다.
- 작은 multipart native header는 stack에 두고 큰 배열만 `ArrayPool<ZlinkMsg>`에서 빌리며, Core가 pending operation을 만든 경우에만 completion을 생성하는 현재 tree 후보를 전체 r3로 확인했다. 42/42 case가 완료됐고 STREAM과 PUBSUB WSS는 개선됐지만 회차 변동과 perf fairness 변경이 함께 있어 library 변경의 독립 A/B는 남아 있다. `perf_dotnet_multi_linux_20260830_090635_effective-optimizations-r3.txt`

#### 9.2.1 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미달(29.74%; 후보 r3, 지연 105.101x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 241.9/813.4 Kmsg/s, 평균 지연 98.690/0.939 ms. `perf_dotnet_multi_linux_20260830_090635_effective-optimizations-r3.txt` |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(35.87%; 후보 r3, 지연 1.632x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 74.4/207.5 Kops/s, 평균 지연 1.368/0.838 ms. 같은 report. |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미달(63.73%; 후보 r3, 지연 41.913x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 52.4/82.2 Kops/s, 평균 지연 24.226/0.578 ms. 같은 report. |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(53.98%; 후보 r3, 지연 1.408x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 86.0/159.3 Kops/s, 평균 지연 1.246/0.885 ms. 같은 report. |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미달(54.93%; 후보 r3, 지연 76.189x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 50.3/91.5 Kops/s, 평균 지연 37.104/0.487 ms. 같은 report. |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미달(36.74%; 후보 r3, 지연 1.784x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 280.8/764.4 Kmsg/s, 평균 지연 1859.573/1042.517 ms. 같은 report. |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 통과(80.54%; 후보 r3, 지연 1.481x) | 해당 없음 | 미측정 | 해당 없음 | 처리량 .NET/C 97.7/121.3 Kops/s, 평균 지연 927.765/626.407 ms. 같은 report. 최종 r5 대기. |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미달(36.89%; 후보 r3, 지연 51.017x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 247.4/670.6 Kmsg/s, 평균 지연 160.499/3.146 ms. 같은 report. |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(54.15%; 후보 r3, 지연 0.855x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 77.4/143.0 Kops/s, 평균 지연 3.921/4.586 ms. 같은 report. |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미달(69.98%; 후보 r3, 지연 9.152x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 60.1/85.9 Kops/s, 평균 지연 16.135/1.763 ms. 같은 report. |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(53.35%; 후보 r3, 지연 0.950x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 88.3/165.6 Kops/s, 평균 지연 3.360/3.536 ms. 같은 report. |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미달(73.24%; 후보 r3, 지연 8.249x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 63.4/86.6 Kops/s, 평균 지연 13.991/1.696 ms. 같은 report. |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미달(52.19%; 후보 r3, 지연 0.362x) | 미측정 | 미측정 | 미측정 | 처리량 .NET/C 246.9/473.1 Kmsg/s, 평균 지연 333.898/922.309 ms. 같은 report. |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 통과(86.65%; 후보 r3, 지연 7.617x) | 해당 없음 | 미측정 | 해당 없음 | 처리량 .NET/C 130.2/150.2 Kops/s, 평균 지연 81.914/10.754 ms. 같은 report. 처리량은 통과했지만 지연 상한은 미달이며 최종 r5 대기. |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.3 Java

- perf 경로: `bindings/java/perf`
- Multi 상태: `TCP/WSS 1024B 전체 pattern 개선 후보 최종 r5 완료, 목표 미달 경로 후속 검토 중`
- paired report: C `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260830_022104_final-0146-c-baseline-r5.txt`, Java `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260830_044718_baseline-0146-java-r5.txt`
- 조건: Core 0.14.6(`22e608ccdc`), Release/LTO, duration 5초, runs 5, clients 100, I/O threads 4, TCP/WSS, 1024B. 최종 후보 report `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt`는 14/14 case와 350/350 result line이 모두 완료됐다.
- `MULTI_DEALER_DEALER` 소켓별 pending 객체 캐시 후보는 TCP 504.9 Kmsg/s, WSS 459.9 Kmsg/s로 직전 후보 대비 각각 -2.2%, +3.0%여서 일관된 이득이 없다고 판단해 기각했다. `perf_java_multi_linux_20260830_062646_pending-cache-dd-r3.txt`
- hot path의 `PERF_PART_COUNT` 환경 조회를 시작 시 1회로 고정하고, FFM native message 배열 slice를 재사용했다. fast-token 후보까지 적용한 최신 r3는 TCP 526.6 Kmsg/s, WSS 474.8 Kmsg/s이며 WSS는 최소 목표 70%에 근접했다. `perf_java_multi_linux_20260830_063431_fast-token-dd-r3.txt`
- `MULTI_DEALER_ROUTER_REQREP`는 Java client+C server 분리 진단에서도 TCP 35.6 Kops/s로 C+C 101.6 Kops/s의 35.0%에 머물러 client 요청 경로가 주 병목임을 확인했다. completion batch 16, poll 반환 후 inline completion, atomic fast pending table, socket-local synchronized pending table, worker 64개 후보는 처리량 또는 지연이 악화되어 모두 기각했다.
- 공통 2-part 요청은 binding 전용 native bridge로 두 Core part 호출을 한 FFM 경계로 묶었다. source native message를 직접 넘기고 성공한 part 수를 Java로 반환해 generic part 경로와 같은 부분 성공 ownership을 보존한다. 초기 copy bridge의 on/off paired r3는 TCP 29.4/29.6 Kops/s로 동일했고 WSS 51.8/39.3 Kops/s였으며, direct-source 후보도 TCP 처리량은 노이즈 범위였다. reply callback의 `Message[] + Arrays.asList` 제거 구조는 이후 deferred snapshot으로 대체했다. `perf_java_multi_linux_20260830_071914_native-two-part-request-bridge-dr-reqrep-r3.txt`, `perf_java_multi_linux_20260830_072238_paired-bridge-disabled-dr-reqrep-r3.txt`, `perf_java_multi_linux_20260830_082907_direct-request-sources-r3.txt`
- 기본 DEALER request가 C perf와 달리 매 요청마다 exact transport-pair를 선택하던 의미 불일치를 수정해 `zlink_dealer_request_part` 경로로 복구했다. 명시적 exact request와 ROUTER request는 기존 exact-target 경로를 유지한다. 또한 callback depth 진입이 callback handle 생성 스레드에 잘못 남던 회귀를 실제 callback 본문으로 복구해 blocking API의 순서 의존 오판정을 제거했다.
- reply native message header는 poll/callback 스레드에서 할당되고 completion worker에서 닫히므로 기존 thread-local slot pool로 돌아가지 않았다. 같은 스레드 반환은 기존 무동기화 풀을 유지하고 교차 스레드 반환만 bounded shared pool로 회수했다. r3 후보는 TCP 36.1 Kops/s, WSS 53.0 Kops/s이며 TCP는 직전 34.3 Kops/s 대비 5.2% 개선됐다. `perf_java_multi_linux_20260830_074236_dealer-standard-request-r3.txt`, `perf_java_multi_linux_20260830_074704_cross-thread-msg-slot-pool-r3.txt`
- Core reply callback에서 public `Message`를 생성하던 작업을 native message pair snapshot으로 넘기고 completion worker에서 materialize하도록 변경했다. callback/poller의 FFM segment 생성 비용이 제거되어 paired r3에서 TCP 43.7 Kops/s, WSS 54.3 Kops/s까지 개선됐다. `perf_c_multi_linux_20260830_075822_paired-java-current-r3.txt`, `perf_java_multi_linux_20260830_081036_pooled-deferred-reply-r3.txt`
- 불필요한 동기화 제거 때 함께 사라졌던 send terminal 내부 상태 재사용을 socket-local bounded pool로 복원했다. public `CompletionStage`는 재사용하지 않고 완전히 종료된 내부 `Pending`만 회수한다. RR SENDSEND TCP 단독 r3는 113.8→136.0 Kops/s(+19.5%), WSS는 86.7→96.5 Kops/s(+11.3%)로 개선됐다. 이미 완료된 stage의 `toCompletableFuture()` fast path는 TCP 63.7 Kops/s로 회귀해 제거했다.

#### 9.3.1 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미달(64.64%; 최종 r5, 지연 0.273x) | 미측정 | 미측정 | 미측정 | 처리량 Java/C 525.7/813.4 Kmsg/s, 평균 지연 0.256/0.939 ms. 환경 조회·native slice 반복 할당 제거 및 실제 pending에만 future를 만드는 구조 적용. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(58.30%; 최종 r5, 지연 401.749x) | 미측정 | 미측정 | 미측정 | 내부 send terminal `Pending` 상태 pool 적용 후 처리량 Java/C 121.0/207.5 Kops/s, 평균 지연 336.666/0.838 ms. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 통과(52.14%; 최종 r5, 지연 2.536x) | 미측정 | 미측정 | 미측정 | 처리량 Java/C 42.9/82.2 Kops/s, 평균 지연 1.466/0.578 ms. reply materialization을 callback에서 completion worker로 이관한 pooled native snapshot 경로다. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(59.57%; 최종 r5, 지연 98.440x) | 미측정 | 미측정 | 미측정 | 내부 send terminal `Pending` 상태 pool 적용 후 처리량 Java/C 94.9/159.3 Kops/s, 평균 지연 87.119/0.885 ms. r3 단독 최고치는 최종 중앙값으로 재현되지 않았다. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미달(24.11%; 최종 r5, 지연 4.002x) | 미측정 | 미측정 | 미측정 | 처리량 Java/C 22.1/91.5 Kops/s, 평균 지연 1.949/0.487 ms. r3 개선값을 재현하지 못해 TCP request 경로 후속 진단이 필요하다. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 통과(86.55%; 최종 r5, 지연 1.148x) | 미측정 | 미측정 | 미측정 | 처리량 Java/C 661.6/764.4 Kmsg/s, 평균 지연 1196.375/1042.517 ms. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 통과(94.28%; 최종 r5, 지연 1.508x) | 해당 없음 | 미측정 | 해당 없음 | 처리량 Java/C 114.3/121.3 Kops/s, 평균 지연 944.567/626.407 ms. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미달(50.13%; 최종 r5, 지연 1.634x) | 미측정 | 미측정 | 미측정 | 처리량 Java/C 336.2/670.6 Kmsg/s, 평균 지연 5.140/3.146 ms. 반복값 187.0~444.7 Kmsg/s로 변동이 커 기존 r3 통과값을 재현하지 못했다. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(49.84%; 최종 r5, 지연 87.690x) | 미측정 | 미측정 | 미측정 | 내부 send terminal `Pending` 상태 pool 적용 후 처리량 Java/C 71.3/143.0 Kops/s, 평균 지연 402.145/4.586 ms. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 통과(65.52%; 최종 r5, 지연 1.532x) | 미측정 | 미측정 | 미측정 | 처리량 Java/C 56.3/85.9 Kops/s, 평균 지연 2.701/1.763 ms. reply materialization을 callback에서 completion worker로 이관한 pooled native snapshot 경로다. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(49.29%; 최종 r5, 지연 54.723x) | 미측정 | 미측정 | 미측정 | 내부 send terminal `Pending` 상태 pool 적용 후 처리량 Java/C 81.6/165.6 Kops/s, 평균 지연 193.502/3.536 ms. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 통과(61.36%; 최종 r5, 지연 1.300x) | 미측정 | 미측정 | 미측정 | 처리량 Java/C 53.2/86.6 Kops/s, 평균 지연 2.205/1.696 ms. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미달(55.76%; 최종 r5, 지연 1.897x) | 미측정 | 미측정 | 미측정 | 처리량 Java/C 263.8/473.1 Kmsg/s, 평균 지연 1749.818/922.309 ms. WSS PUBSUB hot path 추가 검토가 필요하다. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 통과(101.53%; 최종 r5, 지연 9.776x) | 해당 없음 | 미측정 | 해당 없음 | 처리량 Java/C 152.5/150.2 Kops/s, 평균 지연 105.132/10.754 ms. 처리량은 통과했지만 지연 상한은 미달이다. `perf_java_multi_linux_20260830_091609_effective-optimizations-final-r5.txt` |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.4 Node

- perf 경로: `bindings/node/perf`
- Multi 상태: `TCP/WSS 1024B 전체 pattern 후보 r3 측정 완료, 추가 개선·최종 r5 대기`
- 2-part echo server의 `Message → Buffer → Message` 왕복 복사를 제거하고 received native `Message`를 public reply builder에 직접 전달했다. native dealer/router reply staging도 heap `std::vector` 대신 2-part inline storage를 사용하며 성공·실패 ownership contract 13개가 통과했다. `perf_node_multi_linux_20260830_085548_direct-reply-small-storage-r3.txt`

#### 9.4.1 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미달(8.54%; 후보 r3, 지연 0.613x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 69.4/813.4 Kmsg/s, 평균 지연 0.576/0.939 ms. 세 회차 중 한 회차가 9.5 Kmsg/s로 흔들려 추가 r5 확인이 필요하다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(18.37%; 후보 r3, 지연 375.847x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 38.1/207.5 Kops/s, 평균 지연 314.960/0.838 ms. 최소 처리량과 지연 목표가 모두 미달이다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미달(48.02%; 후보 r3, 지연 18.972x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 39.5/82.2 Kops/s, 평균 지연 10.966/0.578 ms. 처리량 최소 30%는 통과했지만 TCP 지연 상한 5배를 초과한다. `perf_node_multi_linux_20260830_085548_direct-reply-small-storage-r3.txt` |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(20.19%; 후보 r3, 지연 274.825x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 32.2/159.3 Kops/s, 평균 지연 243.220/0.885 ms. 최소 처리량과 지연 목표가 모두 미달이다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미달(42.12%; 후보 r3, 지연 19.384x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 38.5/91.5 Kops/s, 평균 지연 9.440/0.487 ms. 처리량 최소 30%는 통과했지만 TCP 지연 상한 5배를 초과한다. `perf_node_multi_linux_20260830_085548_direct-reply-small-storage-r3.txt` |
| `tcp` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미달(21.33%; 후보 r3, 지연 0.067x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 163.0/764.4 Kmsg/s, 평균 지연 69.468/1042.517 ms. 최소 처리량 목표가 미달이다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미달(8.04%; 후보 r3, 지연 1.693x) | 해당 없음 | 미측정 | 해당 없음 | 처리량 Node/C 9.7/121.3 Kops/s, 평균 지연 1060.713/626.407 ms. STREAM 전용 callback·TSFN 경로 조사가 필요하다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미달(16.30%; 후보 r3, 지연 0.496x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 109.3/670.6 Kmsg/s, 평균 지연 1.559/3.146 ms. 최소 처리량 목표가 미달이다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(26.83%; 후보 r3, 지연 77.356x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 38.4/143.0 Kops/s, 평균 지연 354.756/4.586 ms. 최소 처리량과 지연 목표가 모두 미달이다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 통과(47.34%; 후보 r3, 지연 4.139x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 40.7/85.9 Kops/s, 평균 지연 7.297/1.763 ms. 처리량 최소 30%와 Node 지연 상한 5배를 통과했으며 최종 r5 대기. `perf_node_multi_linux_20260830_085548_direct-reply-small-storage-r3.txt` |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미달(16.94%; 후보 r3, 지연 83.145x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 28.0/165.6 Kops/s, 평균 지연 294.002/3.536 ms. 최소 처리량과 지연 목표가 모두 미달이다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 통과(44.48%; 후보 r3, 지연 4.692x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 38.5/86.6 Kops/s, 평균 지연 7.958/1.696 ms. 처리량 최소 30%와 Node 지연 상한 5배를 통과했으며 최종 r5 대기. `perf_node_multi_linux_20260830_085548_direct-reply-small-storage-r3.txt` |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미달(28.28%; 후보 r3, 지연 0.255x) | 미측정 | 미측정 | 미측정 | 처리량 Node/C 133.8/473.1 Kmsg/s, 평균 지연 235.195/922.309 ms. 최소 처리량 목표가 미달이다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미달(7.21%; 후보 r3, 지연 98.751x) | 해당 없음 | 미측정 | 해당 없음 | 처리량 Node/C 10.8/150.2 Kops/s, 평균 지연 1061.936/10.754 ms. STREAM 전용 callback·TSFN 경로 조사가 필요하다. `perf_node_multi_linux_20260830_090206_current-remaining-r3.txt` |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.5 Go

- perf 경로: `bindings/go/perf`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.5.1 Multi suite

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
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.6.1 Multi suite

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

### 9.7 Python

- perf 경로: `bindings/python/perf`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

#### 9.7.1 Multi suite

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

| 순서 | 언어 | Multi 상태 | 다음 작업 |
|------|------|------------|-----------|
| 1 | C++ | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 2 | .NET | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 3 | Java | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 4 | Node | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 5 | Go | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 6 | Rust | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |
| 7 | Python | 미측정 | inventory gate와 paired 기준 측정을 시작한다. |

## 11. 측정 기록과 결과

paired 측정을 완료할 때마다 아래 표에 측정 조건과 결과만 한 행으로 추가한다. 실행 과정,
후보 검토, 프로파일과 구현 변경은 이 문서가 있는 폴더의 `log/`에 별도로 기록한다.

| 날짜 | 언어 | suite / 범위 | pair tag | 측정 조건 | 결과 | report |
|------|------|---------------|----------|----------------|------|---------------|
| 2026-08-07 | 전체 | 계획 초기화 | - | Core 0.14.0 release, C 기준과 binding paired 비교, 단일 perf process 조건을 사용한다. | 계획 작성 | 이 문서 |

## 12. 완료 기준

다음 조건을 모두 만족해야 작업을 완료한다.

- runner, 정책, 상세 표의 pattern, transport, size inventory가 일치한다.
- 각 pattern의 최종 판정에 사용한 core 0.14.0 C와 binding paired report가 모두
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
