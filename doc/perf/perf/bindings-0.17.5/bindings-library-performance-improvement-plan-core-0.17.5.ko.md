# core 0.17.5 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-09-09
>
> 작업 기준: `main` (별도 브랜치 없음; 검증된 단위마다 커밋·푸시)
>
> Core 기준 runtime: **0.17.5** — tag `core/v0.17.5`, release revision `766dd1bb6e`
> (fix 커밋 `c72bc2dc63`, D-BP52), GitHub Release 자산(Linux 요구 glibc 2.34). 첫 0.17.5
> 측정에서 runtime resolve를 확인했다: `~/.cache/zlink/core/0.17.5/linux-x64/lib/libzlink.so.0.17.5`,
> provenance(`share/zlink/core-package-provenance.json`) version=0.17.5, revision 766dd1bb6e.
>
> **버전 전환 메모(2026-09-09).** C++(첫 언어)는 core **0.17.4**에서 Single·Multi를 모두
> 측정 완료했다(§9.1, pair tag `c0174-*`). 0.17.5의 변경은 STREAM 대규모 CCU(약 3,500↑,
> 10,000 목표) Auto-HWM 예약 수정과 C perf 러너 진단 기록뿐이고, 1,000-connection STREAM
> throughput은 불변(+0.55 %)이다. 우리 STREAM 측정은 노트북 한계로 **100 CCU**(§3.3)라 이
> 변경이 닿지 않으므로 **C++의 0.17.4 결과는 유효**하다. **.NET부터는 `--core-version 0.17.5`로
> 측정**하며 pair tag는 `c0175-*`를 쓴다.
>
> 이 문서는 core 0.17.5를 기준으로 성능을 확인하고 bindings 라이브러리 성능 개선을 처음부터
> 진행하기 위한 실행 문서다. 이전 계획 문서의 측정값과 완료 판정은 가져오지 않는다.
> 새 C 기준 결과와 각 binding의 새 결과만 이 문서에 기록한다. 이 계획서에는 측정 대상,
> 측정 조건, report 경로, 비교값과 판정만 남긴다. 실행 명령, 후보 검토, 프로파일과 같은
> 과정 설명은 이 문서가 있는 폴더의 `log/`에 기록한다.

## 1. 기준 버전과 시작 상태

이번 작업의 core 기준 버전은 0.17.5이다(C++는 0.17.4에서 측정 완료; 상단 버전 전환 메모 참조).
측정 전에 다음 세 파일의 버전이 모두 같은지 확인한다.

- `VERSION`: `LIBZLINK_VERSION=0.17.5`
- `core/CMakeLists.txt`: `project(zlink VERSION 0.17.5 ...)`
- `core/include/zlink.h`: major, minor, patch values matching 0.17.5

`bindings/tools/local_core_runtime.sh`는 `VERSION`의 값을 이용해 GitHub의
`core/v0.17.5` release asset을 기존 release 절차로 가져오고 versioned
runtime 경로를 선택한다. 따라서 파일 이름이나 `Perf runtime libzlink: ...` 경로만
보고 판정하지 않는다. runner 또는 binding의 public version API와
`share/zlink/core-package-provenance.json`이 보고한 실제 runtime 버전도
0.17.5인지 확인한다.

측정을 시작할 때는 Core source를 다시 build하지 않는다. 모든 perf runner는 기본적으로
LOCAL Core build를 선택한다. 공식 측정에서는 runner에 `--core-version 0.17.5`를
전달해 검증된 release runtime을 선택하며, 이 option이 release prefix와 package provenance를
resolve하고 verify한다. `ZLINK_CORE_SOURCE`를 명시적으로 export한 경우에는 그 값이 runner의
기본 선택보다 우선한다. `core/build`와 현재 source 변경은 측정 runtime을 구성하지 않는다.
다른 버전의 local package나 오래된 runtime을 사용한 결과도 이 문서의 기준값으로 사용하지
않는다.

모든 성능 셀은 `미측정`에서 시작한다. 상세 표에는 현재 binding runner에 실제로 등록된
pattern만 포함한다. 공식 C runner에만 있고 binding runner에 없는 pattern은 이 계획의
측정 대상에서 제외한다. 이전 문서와 이전 report는 병목 후보를 찾는 참고 자료로만 사용하며,
core 0.17.5의 통과 비율이나 완료 근거로 사용하지 않는다.

언어별 pattern 목록이 다른 것은 Core C API 또는 binding public contract가 언어별로 다르다는
뜻이 아니다. 모든 binding은 같은 Core C API를 감싸지만, 각 언어의 perf runner가 현재
구현하고 등록한 측정 scenario가 다를 수 있다. 따라서 이 문서의 언어별 차이는 public API
차이가 아니라 perf runner 구현 범위의 차이로 해석한다.

### 1.1 core 0.17.4·0.17.5에서 바뀐 것

성능 확인 대상이 되는 변경은 다음과 같다(`core/CHANGELOG.md` [0.17.4], [0.17.5]).
각 항목은 어느 셀에서 관측되는지만 적는다. 판정은 §8의 규칙과 §9의 표로 한다.

| core 변경 | 관측되는 셀 |
|---|---|
| (0.17.4) attach 시 context 전체 Auto-HWM 동기 재계산 제거(증분 확장, 수렴은 debounce 재계산) | 연결 수립 구간(§3.3의 client 수 안에서) |
| (0.17.4) STREAM decoder/encoder read target이 첫 full read에서 2배 성장 | 65536 B 이상 수신 셀 (tcp·ws·wss·tls) |
| (0.17.4) WS/WSS bounded gather — 큰 body를 header batch와 한 write로 제출 | ws·wss의 routed 큰 payload 셀과 그 latency |
| (0.17.4) context 종료를 monitor teardown보다 먼저 게시, blocking receive가 매 턴 ETERM 관측 | 러너 종료·drain 경로(`status: complete` 판정) |
| (0.17.4) 효과가 없던 STREAM gather 환경변수 3개 제거(`ZLINK_ASIO_STREAM_DISABLE_GATHER`, `..._GATHER_THRESHOLD`, `..._TINY_GATHER_THRESHOLD`) | 이 변수를 설정하는 러너·스크립트 |
| (0.17.4) Linux release 자산을 Ubuntu 22.04에서 빌드(요구 glibc 2.38 → 2.34) | 자산 수신과 provenance 확인 |
| (0.17.5) STREAM accept 시 Auto-HWM 예약을 예약 위상의 effective budget으로 판정 — Balanced에서 약 3,500 연결 뒤 `ENOBUFS`로 거부되던 것 해소, STREAM echo가 10,000 TCP 연결 도달(64 B 396.6 kops, 6 크기 통과). **1,000-connection throughput 불변(+0.55 %)** | 고CCU STREAM(≥약 3,500). **우리 측정은 100 CCU(§3.3)라 이 경로에 닿지 않음 → C++ 0.17.4 STREAM 결과 유효** |
| (0.17.5) C perf 러너가 monitor event timing·START-wait errno·dirty-prefix provenance를 report에 기록 | 연결 barrier 실패 진단(throughput 무관) |

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

비교 기준은 같은 core 0.17.5 runtime으로 실행한 `bindings/c/perf` 결과다. 같은 suite,
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
96.3%였고 routed one-way와 multi routed echo도 비슷했다. 다만 현재 core 0.17.5의
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
근거로 쓰지 않으며 Node와 같은 60% 중앙값 목표에서 시작한다. 이후 현재 core 0.17.5의
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

core 0.17.5의 현재 multi runner 기본값을 따른다. 이전 표의 256 KiB는 제거하고
4 KiB를 추가한다.

| 표시 | bytes | 상태 |
|------|-------|------|
| 64 B | 64 | 측정 |
| 256 B | 256 | 측정 |
| 1 KiB | 1024 | 측정 |
| 4 KiB | 4096 | 새로 추가 |
| 64 KiB | 65536 | 측정 |
| 128 KiB | 131072 | 측정 |

### 3.3 STREAM client 수

`MULTI_STREAM`은 **client 100개로 측정한다**. 측정 호스트가 노트북이라 그 이상의 동시
접속에서는 측정 자체가 무너진다. C와 binding 모두 같은 100으로 맞추고, report의 실제
client 수가 100인지 확인한다(memory guard가 줄였으면 그 결과는 paired 비교에 쓰지 않는다).
client 수를 늘려 잡은 결과는 이 문서의 판정에 사용하지 않는다.

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
| 조건 | suite, pattern, transport, size, duration, runs, client 수(STREAM은 §3.3의 100), I/O thread 수 |
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
2. GitHub `core/v0.17.5` release asset과 package provenance를 준비하고 재현
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

- `미측정`: 같은 조건의 core 0.17.5 C 결과와 binding 결과를 아직 비교하지 않았다.
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

> **측정 조건(2026-09-09~, 전 언어 공통).** 이번 캠페인의 paired 측정은 GitHub release
> runtime(`core_source=release`)을 `--core-version <ver> --duration 5 --runs 1`, io_threads=1,
> auto-HWM balanced 조건에서 실행한다. **버전: C++는 `0.17.4`(tag `c0174-*`), .NET부터는 `0.17.5`
> (tag `c0175-*`)** — 상단 버전 전환 메모 참조(0.17.5는 STREAM 고CCU만 바뀌어 100 CCU 측정에는
> 영향 없음). runtime 경로 예: `~/.cache/zlink/core/<ver>/linux-x64/lib/libzlink.so.<ver>`.
> C와 binding에 같은 pair tag(`c017N-<lang>-<suite>-<transport>`)를
> 부여하고 순차(비병렬) 실행한다. 상세 표의 size 셀에는 throughput ratio(%)를, 메모 열에는
> `판정 aggregate(mean tput%) / lat(median×) · pair tag · 결과 TSV`를 적는다. 원시 반복값·report
> 경로·Effective Options는 `log/results-<lang>-<suite>.tsv`에 있다.
> **caveat:** release runtime은 monitor snapshot ABI가 없어 러너가 auto-HWM Detail 출력을
> 끈다(`PERF_PRINT_AUTO_HWM_DETAIL=0`). 따라서 이번 캠페인은 `MsgUnit(B)` 일치를 report로
> 직접 확인하지 못한다. C·binding이 같은 release runtime과 같은 auto-HWM balanced 프로파일,
> 같은 Effective Options로 실행되므로 effective HWM은 동일한 것으로 본다.

<!--SUMMARY_START-->

#### 언어별 C 대비 평균 (진행 요약, 자동 갱신)

| 언어 | Core | Single 평균(tput% / lat중앙×) | Multi 평균(tput% / lat중앙×) |
|------|------|------------------------------|------------------------------|
| cpp | 0.17.4 | 86.5% / 1.03× | 99.0% / 1.02× |
| dotnet | 0.17.5 | 84.5% / 0.92× | 90.9% / 0.92× |
| java | 0.17.5 | 92.7% / 1.00× | 70.5% / 1.21× |
| node | 0.17.5 | 72.8% / 2.68× (42 해당없음) | 41.6% / 2.83× |
| go | 0.17.5 | 미측정 | 미측정 |
| rust | 0.17.5 | 미측정 | 미측정 |
| python | 0.17.5 | 미측정 | 미측정 |

> 위는 각 (언어, suite)의 **모든 측정 셀** throughput ratio 산술평균과 latency ratio
> 중앙값이다(전 transport·pattern·size 통합, 참고용 개괄). 셀별 판정과 pattern 그룹 목표는
> §9의 상세 표를 따른다. C++는 0.17.4, 나머지는 0.17.5 baseline. NA는 실패/미측정 셀 수.

<!--SUMMARY_END-->

### 9.1 C++

- perf 경로: `bindings/cpp/perf`
- Single 상태: `측정 완료(2026-09-09)` — 6 transport × 7 pattern paired 완료(42셀 모두 complete).
  통과 16 / 미달 26. 미달은 2단계 개선 대상(임시 상태). 특이: ws·wss·tls의 1024B routed
  req/reply 급락, inproc routed·req/reply 대폭 미달(memory-copy 상한·snapshot 비용).
- Multi 상태: `측정 완료(2026-09-09, clean 호스트 재측정, 실패 수정 반영)` — 4 transport × 7 pattern,
  clients=100. 통과 12 / 미달 16 / 미측정 0. 최초 3셀(131072B routed req/reply, non_zero_exit_1:
  wss DR·RR REQREP, tls DR REQREP)이 실패했으나 **원인이 reqrep 클라이언트 harness가 표준
  send_drain_timeout_ms(5000ms) 대신 1000ms drain을 쓴 parity 버그**임을 확인해 수정(6af8c20906) 후
  재측정으로 해소(75.6·82.8·72.6%). Core 버그 아님. 최초 측정은 외부 worktree perf와 동시
  실행돼 무효화하고 재측정했다.
- 다음 작업: .NET Single·Multi paired 측정으로 이동(C++ 미달·실패는 2단계 개선에서 처리).

> **C++ routed 개선 시도 — 개선 없음/보류(2026-09-09).** ws DEALER_ROUTER(76.2%) 등 작은-payload routed를 callgrind로
> 프로파일했다. 비용은 Core 라우팅(msg_t::copy, router recv, routing-id copy)에 낮게 분산돼 있고 C++ **바인딩 런타임에는
> 제거 가능한 지배적 hot spot이 없다**(이미 near-optimal, 전체 86~99%). 벤치마크 harness의 수신 객체 수명을 손대는
> 변경을 시험했으나 (a) 측정 의미를 C와 다르게 만들 소지가 있고 (b) 실질 이득이 없어 **되돌렸다**. 수치만 올리는 harness
> 튜닝은 목표가 아니다(동일 측정 의미로 라이브러리 성능을 개선하는 것이 목표). C++ pool 재도입 금지(§7.6) 준수. routed
> 작은-payload 격차는 Core 라우팅 + 전송 framing 고유 비용으로 본다.

#### 9.1.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 80.5% | 85.4% | 98.1% | 101.9% | 99.4% | 98.0% | 미달 93.9%/lat0.99× · c0174-cpp-single-tcp |
| `tcp` | `PUBSUB` | 80.4% | 85.5% | 103.8% | 98.0% | 100.0% | 99.6% | 미달 94.5%/lat0.99× · c0174-cpp-single-tcp |
| `tcp` | `DEALER_DEALER` | 87.6% | 85.1% | 105.3% | 98.6% | 96.5% | 97.9% | 통과 95.2%/lat0.97× · c0174-cpp-single-tcp |
| `tcp` | `DEALER_ROUTER` | 80.8% | 83.3% | 101.3% | 101.1% | 103.3% | 98.0% | 통과 94.6%/lat0.98× · c0174-cpp-single-tcp |
| `tcp` | `DEALER_ROUTER_REQREP` | 74.0% | 67.3% | 90.4% | 95.6% | 102.4% | 102.5% | 통과 88.7%/lat1.10× · c0174-cpp-single-tcp |
| `tcp` | `ROUTER_ROUTER` | 80.9% | 88.7% | 93.0% | 97.2% | 97.1% | 97.4% | 통과 92.4%/lat1.03× · c0174-cpp-single-tcp |
| `tcp` | `ROUTER_ROUTER_REQREP` | 64.2% | 63.4% | 85.3% | 99.8% | 102.1% | 102.8% | 통과 86.3%/lat1.40× · c0174-cpp-single-tcp |
| `ws` | `PAIR` | 75.8% | 77.1% | 83.1% | 96.0% | 98.6% | 100.5% | 미달 88.5%/lat1.02× · c0174-cpp-single-ws |
| `ws` | `PUBSUB` | 77.8% | 75.2% | 84.6% | 113.3% | 125.2% | 136.9% | 통과 102.2%/lat1.25× · c0174-cpp-single-ws |
| `ws` | `DEALER_DEALER` | 77.7% | 68.1% | 77.1% | 82.2% | 89.7% | 88.6% | 미달 80.6%/lat1.21× · c0174-cpp-single-ws |
| `ws` | `DEALER_ROUTER` | 66.8% | 65.7% | 74.5% | 76.9% | 82.5% | 91.1% | 미달 76.2%/lat1.43× · c0174-cpp-single-ws |
| `ws` | `DEALER_ROUTER_REQREP` | 77.5% | 77.6% | 24.5% | 94.7% | 96.6% | 99.4% | 미달 78.4%/lat1.21× · c0174-cpp-single-ws |
| `ws` | `ROUTER_ROUTER` | 80.4% | 71.5% | 83.5% | 93.5% | 94.7% | 91.5% | 통과 85.9%/lat1.15× · c0174-cpp-single-ws |
| `ws` | `ROUTER_ROUTER_REQREP` | 79.0% | 65.5% | 23.0% | 89.8% | 95.2% | 97.5% | 미달 75.0%/lat1.26× · c0174-cpp-single-ws |
| `wss` | `PAIR` | 77.3% | 82.2% | 96.7% | 97.2% | 99.8% | 95.3% | 미달 91.4%/lat1.06× · c0174-cpp-single-wss |
| `wss` | `PUBSUB` | 77.0% | 84.4% | 98.9% | 97.4% | 94.7% | 84.7% | 미달 89.5%/lat1.10× · c0174-cpp-single-wss |
| `wss` | `DEALER_DEALER` | 84.6% | 82.7% | 84.9% | 92.4% | 88.6% | 99.8% | 미달 88.8%/lat1.02× · c0174-cpp-single-wss |
| `wss` | `DEALER_ROUTER` | 75.7% | 81.3% | 100.7% | 101.9% | 103.1% | 103.9% | 통과 94.4%/lat1.10× · c0174-cpp-single-wss |
| `wss` | `DEALER_ROUTER_REQREP` | 81.1% | 79.4% | 19.2% | 101.4% | 101.3% | 99.0% | 미달 80.2%/lat1.22× · c0174-cpp-single-wss |
| `wss` | `ROUTER_ROUTER` | 77.9% | 81.4% | 91.8% | 96.7% | 98.1% | 102.3% | 통과 91.4%/lat1.06× · c0174-cpp-single-wss |
| `wss` | `ROUTER_ROUTER_REQREP` | 74.3% | 78.1% | 28.5% | 103.3% | 108.2% | 107.0% | 미달 83.2%/lat1.11× · c0174-cpp-single-wss |
| `tls` | `PAIR` | 79.0% | 92.7% | 101.9% | 97.1% | 96.1% | 96.5% | 미달 93.9%/lat1.02× · c0174-cpp-single-tls |
| `tls` | `PUBSUB` | 78.6% | 94.6% | 102.8% | 97.7% | 94.8% | 83.6% | 미달 92.0%/lat1.04× · c0174-cpp-single-tls |
| `tls` | `DEALER_DEALER` | 88.4% | 88.9% | 109.5% | 93.1% | 93.3% | 94.6% | 미달 94.6%/lat1.06× · c0174-cpp-single-tls |
| `tls` | `DEALER_ROUTER` | 67.1% | 77.3% | 91.9% | 96.0% | 101.0% | 101.7% | 통과 89.2%/lat1.08× · c0174-cpp-single-tls |
| `tls` | `DEALER_ROUTER_REQREP` | 71.8% | 71.7% | 53.7% | 102.8% | 98.2% | 103.8% | 미달 83.7%/lat1.10× · c0174-cpp-single-tls |
| `tls` | `ROUTER_ROUTER` | 97.5% | 98.5% | 107.6% | 107.9% | 108.0% | 100.4% | 통과 103.3%/lat0.96× · c0174-cpp-single-tls |
| `tls` | `ROUTER_ROUTER_REQREP` | 63.7% | 75.0% | 57.1% | 94.6% | 98.4% | 97.0% | 미달 81.0%/lat1.11× · c0174-cpp-single-tls |
| `inproc` | `PAIR` | 83.3% | 82.1% | 76.6% | 87.0% | 93.6% | 99.2% | 미달 87.0%/lat0.81× · c0174-cpp-single-inproc |
| `inproc` | `PUBSUB` | 82.4% | 84.7% | 83.9% | 72.3% | 141.8% | 113.8% | 통과 96.5%/lat0.80× · c0174-cpp-single-inproc |
| `inproc` | `DEALER_DEALER` | 84.4% | 93.5% | 91.8% | 22.7% | 38.9% | 49.1% | 미달 63.4%/lat1.50× · c0174-cpp-single-inproc |
| `inproc` | `DEALER_ROUTER` | 81.7% | 87.1% | 90.0% | 94.0% | 39.6% | 102.6% | 미달 82.5%/lat1.10× · c0174-cpp-single-inproc |
| `inproc` | `DEALER_ROUTER_REQREP` | 64.0% | 63.2% | 65.3% | 55.0% | 58.8% | 45.3% | 미달 58.6%/lat2.96× · c0174-cpp-single-inproc |
| `inproc` | `ROUTER_ROUTER` | 91.4% | 89.5% | 86.0% | 21.2% | 86.5% | 95.6% | 미달 78.4%/lat0.92× · c0174-cpp-single-inproc |
| `inproc` | `ROUTER_ROUTER_REQREP` | 49.2% | 48.8% | 47.3% | 30.3% | 40.9% | 39.2% | 미달 42.6%/lat13.08× · c0174-cpp-single-inproc |
| `ipc` | `PAIR` | 80.1% | 83.9% | 98.0% | 96.3% | 97.2% | 97.2% | 미달 92.1%/lat1.02× · c0174-cpp-single-ipc |
| `ipc` | `PUBSUB` | 76.8% | 83.5% | 100.8% | 133.4% | 98.4% | 98.7% | 통과 98.6%/lat1.05× · c0174-cpp-single-ipc |
| `ipc` | `DEALER_DEALER` | 76.9% | 83.0% | 83.8% | 88.9% | 91.2% | 90.6% | 미달 85.7%/lat1.10× · c0174-cpp-single-ipc |
| `ipc` | `DEALER_ROUTER` | 70.8% | 80.4% | 104.3% | 102.9% | 106.3% | 99.1% | 통과 94.0%/lat0.96× · c0174-cpp-single-ipc |
| `ipc` | `DEALER_ROUTER_REQREP` | 77.8% | 74.1% | 71.3% | 98.9% | 88.1% | 105.4% | 통과 85.9%/lat1.08× · c0174-cpp-single-ipc |
| `ipc` | `ROUTER_ROUTER` | 82.9% | 85.5% | 90.1% | 96.0% | 98.8% | 103.3% | 통과 92.8%/lat1.03× · c0174-cpp-single-ipc |
| `ipc` | `ROUTER_ROUTER_REQREP` | 65.1% | 62.4% | 65.9% | 103.3% | 100.5% | 109.7% | 미달 84.5%/lat1.38× · c0174-cpp-single-ipc |

#### 9.1.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 137.3% | 168.3% | 273.8% | 252.9% | 320.1% | 146.7% | 통과 216.5%/lat0.35× · c0174-cpp-multi-tcp |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 203.4% | 156.9% | 162.5% | 173.9% | 154.5% | 65.5% | 통과 152.8%/lat0.78× · c0174-cpp-multi-tcp |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 119.8% | 89.5% | 154.7% | 162.0% | 184.9% | 96.0% | 통과 134.5%/lat0.34× · c0174-cpp-multi-tcp |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 123.0% | 118.5% | 138.2% | 145.2% | 131.3% | 21.1% | 통과 112.9%/lat0.88× · c0174-cpp-multi-tcp |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 66.3% | 79.3% | 72.1% | 79.0% | 85.1% | 91.3% | 미달 78.8%/lat0.89× · c0174-cpp-multi-tcp |
| `tcp` | `MULTI_PUBSUB` | 87.0% | 81.8% | 86.3% | 89.8% | 104.1% | 108.6% | 미달 92.9%/lat1.01× · c0174-cpp-multi-tcp |
| `tcp` | `MULTI_STREAM` | 76.8% | 93.5% | 94.8% | 해당 없음 | 90.4% | 해당 없음 | 미달 88.9%/lat1.09× · c0174-cpp-multi-tcp |
| `ws` | `MULTI_DEALER_DEALER` | 80.7% | 51.8% | 66.1% | 79.1% | 101.3% | 79.3% | 미달 76.4%/lat0.69× · c0174-cpp-multi-ws |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 129.6% | 110.8% | 77.2% | 99.4% | 58.8% | 84.3% | 통과 93.4%/lat1.19× · c0174-cpp-multi-ws |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 57.4% | 52.0% | 70.9% | 135.8% | 97.8% | 105.1% | 미달 86.5%/lat3.13× · c0174-cpp-multi-ws |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 117.6% | 94.2% | 67.1% | 49.7% | 69.9% | 68.4% | 미달 77.8%/lat1.62× · c0174-cpp-multi-ws |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 72.2% | 73.3% | 89.1% | 129.0% | 119.6% | 107.7% | 미달 98.5%/lat2.08× · c0174-cpp-multi-ws |
| `ws` | `MULTI_PUBSUB` | 101.8% | 88.8% | 91.1% | 94.4% | 106.9% | 106.8% | 통과 98.3%/lat1.06× · c0174-cpp-multi-ws |
| `ws` | `MULTI_STREAM` | 82.0% | 122.5% | 97.9% | 해당 없음 | 117.2% | 해당 없음 | 통과 104.9%/lat0.94× · c0174-cpp-multi-ws |
| `wss` | `MULTI_DEALER_DEALER` | 78.0% | 77.0% | 88.3% | 78.7% | 97.4% | 122.3% | 미달 90.3%/lat0.71× · c0174-cpp-multi-wss |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 86.4% | 88.9% | 87.9% | 41.4% | 106.9% | 94.9% | 미달 84.4%/lat1.02× · c0174-cpp-multi-wss |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 47.5% | 62.4% | 64.9% | 107.4% | 75.4% | 75.6% | 미달 72.2%/lat5.93× · c0174-cpp-multi |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 84.8% | 81.6% | 58.7% | 85.2% | 62.7% | 71.7% | 미달 74.1%/lat1.18× · c0174-cpp-multi-wss |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 76.3% | 56.8% | 60.8% | 107.0% | 78.1% | 82.8% | 미달 77.0%/lat1.65× · c0174-cpp-multi |
| `wss` | `MULTI_PUBSUB` | 63.6% | 100.6% | 96.1% | 101.3% | 123.7% | 84.1% | 미달 94.9%/lat1.06× · c0174-cpp-multi-wss |
| `wss` | `MULTI_STREAM` | 92.9% | 91.1% | 108.6% | 해당 없음 | 116.9% | 해당 없음 | 통과 102.4%/lat1.00× · c0174-cpp-multi-wss |
| `tls` | `MULTI_DEALER_DEALER` | 76.1% | 82.6% | 100.7% | 76.5% | 94.3% | 92.2% | 미달 87.1%/lat1.04× · c0174-cpp-multi-tls |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 100.7% | 98.6% | 83.0% | 97.5% | 112.4% | 101.3% | 통과 98.9%/lat1.28× · c0174-cpp-multi-tls |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 60.4% | 64.8% | 61.4% | 401.4% | 51.9% | 72.6% | 통과 118.8%/lat0.85× · c0174-cpp-multi |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 88.8% | 81.5% | 84.8% | 57.9% | 113.8% | 109.5% | 통과 89.4%/lat1.05× · c0174-cpp-multi-tls |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 38.2% | 56.9% | 61.3% | 68.1% | 72.7% | 74.6% | 미달 62.0%/lat0.92× · c0174-cpp-multi-tls |
| `tls` | `MULTI_PUBSUB` | 81.9% | 82.0% | 86.5% | 93.3% | 112.7% | 93.9% | 미달 91.7%/lat1.04× · c0174-cpp-multi-tls |
| `tls` | `MULTI_STREAM` | 150.5% | 110.0% | 110.9% | 해당 없음 | 119.3% | 해당 없음 | 통과 122.7%/lat0.87× · c0174-cpp-multi-tls |

> **C++ Multi 실패 — 해소됨(2026-09-09, 6af8c20906).** wss·tls의 131072B routed request/reply
> (`MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP`)에서 C++ 바인딩 multi 러너가
> `non_zero_exit_1`(status partial)로 실패했다. **근본 원인은 Core가 아니라 C++ perf harness의
> parity 버그**: reqrep 클라이언트만 drain deadline을 `max(1000, rcvtimeo_ms*4)`(기본 1000ms)로 써서,
> ~135 in-flight 대형 요청이 1000ms 안에 drain되지 못하고 outstanding!=0 → return false로 실패했다
> (이어진 socket close로 ESHUTDOWN 관측, 부수 결과). C 러너·C++ 서버·dealer_dealer 클라이언트는 모두
> 표준 `send_drain_timeout_ms`(기본 5000ms)를 쓴다. reqrep도 같은 값으로 정렬하니 세 셀이 정상 완료
> (75.6·82.8·72.6%). 바인딩 라이브러리·Core는 무변경(회귀테스트 불요; 바인딩 drain 계약은 기존
> `test_cpp_contract_completion_drain_order.cpp`가 커버).

### 9.2 .NET

- perf 경로: `bindings/dotnet/perf` (core **0.17.5**, pair tag `c0175-*`)
- Single 상태: `측정 완료(2026-09-09)` — 6 transport × 7 pattern, 42셀 모두 complete(실패 0).
  통과 28 / 미달 14. 미달: tcp req/reply, inproc·ipc의 다수(작은 payload managed/native 전환
  비용; local transport는 §2.1 예외 목표 적용). 2단계 개선 대상.
- Multi 상태: `측정 완료(2026-09-09, 원샷)` — 4 transport × 7 pattern, clients=100.
  통과 19 / 미달 9 / 미측정 0. 최초 1셀(ws MULTI_ROUTER_ROUTER_SENDSEND 65536·131072B)이
  C multi 러너의 `non_zero_exit_1 at AUTO_HWM_DETAIL`로 실패했으나, **원인이 multi 러너에
  release용 `PERF_PRINT_AUTO_HWM_DETAIL=0` 가드 누락임을 확인해 수정(9f2a977d59)** 후 재측정으로
  해소(통과). .NET report에 `META,core_version` 라인이 없어 status에 `core_meta_missing`을 부기했으나
  package provenance·console로 release 0.17.5 확인(양성).
- 다음 작업: Java Single·Multi paired 측정.

> **.NET routed request/reply 개선 시도 — 보류(2026-09-09).** DEALER_ROUTER_REQREP·ROUTER_ROUTER_REQREP가
> C 대비 낮아(single tcp 63.1/56.4%) 진단했다. 병목은 요청당 managed 할당(RequestCompletionEntry·Task·
> reply builder/collection·ReplyToken·ReceivedReplyContext 등, Gen0 GC 151~187 MB/s)과 **2-part 왕복당 약 30회의
> P/Invoke 경계 + CLR object header/JIT/GC 고유 비용**이다(Core·wire·routing 결함 없음, 분류 B). 계약 보존
> 할당 축소 pass(ReceivedReplyContext 제거·노출 안 된 multipart wrapper 재사용·indexed loop)를 구현해 allocation
> rate는 −6.2% 줄였으나 **throughput이 오히려 회귀**(DEALER_ROUTER_REQREP −3.76%, ROUTER_ROUTER_REQREP −3.75%,
> PAIR −5.02%)했다. 할당이 throughput 병목이 아니고 P/Invoke 경계 비용이 지배적이라, §7.7에 따라 변경을 되돌리고
> 보류로 둔다. public 계약·테스트는 훼손하지 않았다(변경은 전량 revert). inproc/ipc routed는 §2.1 예외/memory-copy
> 상한과 함께 재검토 대상.

#### 9.2.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 43.2% | 49.8% | 76.2% | 141.7% | 121.0% | 109.5% | 통과 90.2%/lat0.94× · c0175-dotnet-single-tcp |
| `tcp` | `PUBSUB` | 47.4% | 51.3% | 85.9% | 215.0% | 198.0% | 171.1% | 통과 128.1%/lat1.32× · c0175-dotnet-single-tcp |
| `tcp` | `DEALER_DEALER` | 42.8% | 45.3% | 64.6% | 162.1% | 139.3% | 105.1% | 통과 93.2%/lat0.64× · c0175-dotnet-single-tcp |
| `tcp` | `DEALER_ROUTER` | 41.7% | 45.0% | 64.7% | 146.2% | 125.4% | 108.9% | 통과 88.6%/lat0.82× · c0175-dotnet-single-tcp |
| `tcp` | `DEALER_ROUTER_REQREP` | 44.4% | 42.4% | 59.6% | 60.4% | 78.7% | 93.3% | 미달 63.1%/lat1.19× · c0175-dotnet-single-tcp |
| `tcp` | `ROUTER_ROUTER` | 42.7% | 48.9% | 63.2% | 145.2% | 125.9% | 107.5% | 통과 88.9%/lat1.09× · c0175-dotnet-single-tcp |
| `tcp` | `ROUTER_ROUTER_REQREP` | 36.6% | 36.8% | 54.8% | 59.3% | 70.8% | 80.2% | 미달 56.4%/lat1.95× · c0175-dotnet-single-tcp |
| `ws` | `PAIR` | 42.1% | 46.4% | 68.3% | 137.4% | 116.6% | 112.9% | 통과 87.3%/lat0.09× · c0175-dotnet-single-ws |
| `ws` | `PUBSUB` | 49.6% | 50.6% | 67.0% | 178.2% | 184.2% | 154.1% | 통과 113.9%/lat0.93× · c0175-dotnet-single-ws |
| `ws` | `DEALER_DEALER` | 46.0% | 46.8% | 60.9% | 168.0% | 151.7% | 112.8% | 통과 97.7%/lat0.08× · c0175-dotnet-single-ws |
| `ws` | `DEALER_ROUTER` | 42.5% | 45.3% | 57.4% | 154.9% | 148.0% | 122.4% | 통과 95.1%/lat0.10× · c0175-dotnet-single-ws |
| `ws` | `DEALER_ROUTER_REQREP` | 48.3% | 98.9% | 57.8% | 69.6% | 77.4% | 89.4% | 통과 73.6%/lat1.20× · c0175-dotnet-single-ws |
| `ws` | `ROUTER_ROUTER` | 47.3% | 46.9% | 55.6% | 158.0% | 132.2% | 116.4% | 통과 92.7%/lat0.07× · c0175-dotnet-single-ws |
| `ws` | `ROUTER_ROUTER_REQREP` | 47.7% | 89.1% | 79.2% | 72.1% | 82.2% | 86.0% | 통과 76.0%/lat1.23× · c0175-dotnet-single-ws |
| `wss` | `PAIR` | 43.8% | 57.2% | 126.8% | 166.9% | 159.0% | 149.6% | 통과 117.2%/lat0.06× · c0175-dotnet-single-wss |
| `wss` | `PUBSUB` | 48.1% | 56.3% | 121.9% | 133.0% | 114.3% | 96.7% | 통과 95.0%/lat0.22× · c0175-dotnet-single-wss |
| `wss` | `DEALER_DEALER` | 42.5% | 53.3% | 110.1% | 167.6% | 162.6% | 152.0% | 통과 114.7%/lat0.35× · c0175-dotnet-single-wss |
| `wss` | `DEALER_ROUTER` | 43.0% | 53.3% | 101.8% | 159.7% | 160.7% | 146.4% | 통과 110.8%/lat0.09× · c0175-dotnet-single-wss |
| `wss` | `DEALER_ROUTER_REQREP` | 43.7% | 143.2% | 80.0% | 93.0% | 108.1% | 135.8% | 통과 100.6%/lat0.81× · c0175-dotnet-single-wss |
| `wss` | `ROUTER_ROUTER` | 46.2% | 54.6% | 109.5% | 157.7% | 167.3% | 155.5% | 통과 115.1%/lat0.05× · c0175-dotnet-single-wss |
| `wss` | `ROUTER_ROUTER_REQREP` | 39.8% | 125.7% | 148.7% | 88.2% | 106.5% | 123.8% | 통과 105.5%/lat0.85× · c0175-dotnet-single-wss |
| `tls` | `PAIR` | 40.8% | 59.2% | 172.5% | 155.2% | 158.1% | 151.1% | 통과 122.8%/lat1.40× · c0175-dotnet-single-tls |
| `tls` | `PUBSUB` | 41.5% | 66.8% | 176.7% | 103.7% | 103.4% | 98.4% | 통과 98.4%/lat1.42× · c0175-dotnet-single-tls |
| `tls` | `DEALER_DEALER` | 33.5% | 39.5% | 109.0% | 138.2% | 145.8% | 143.8% | 통과 101.6%/lat1.34× · c0175-dotnet-single-tls |
| `tls` | `DEALER_ROUTER` | 41.2% | 49.4% | 131.8% | 146.5% | 154.6% | 151.7% | 통과 112.5%/lat0.76× · c0175-dotnet-single-tls |
| `tls` | `DEALER_ROUTER_REQREP` | 41.6% | 44.5% | 91.9% | 80.6% | 89.9% | 111.1% | 통과 76.6%/lat0.97× · c0175-dotnet-single-tls |
| `tls` | `ROUTER_ROUTER` | 41.9% | 50.5% | 95.9% | 137.9% | 141.2% | 141.4% | 통과 101.5%/lat0.43× · c0175-dotnet-single-tls |
| `tls` | `ROUTER_ROUTER_REQREP` | 31.2% | 38.2% | 105.0% | 78.6% | 90.0% | 122.8% | 통과 77.6%/lat0.96× · c0175-dotnet-single-tls |
| `inproc` | `PAIR` | 49.3% | 51.7% | 55.6% | 21.4% | 20.0% | 24.3% | 미달 37.1%/lat2.83× · c0175-dotnet-single-inproc |
| `inproc` | `PUBSUB` | 56.5% | 65.7% | 64.4% | 148.1% | 112.1% | 49.8% | 미달 82.8%/lat3.07× · c0175-dotnet-single-inproc |
| `inproc` | `DEALER_DEALER` | 54.0% | 55.4% | 54.2% | 24.8% | 67.3% | 82.8% | 통과 56.4%/lat1.81× · c0175-dotnet-single-inproc |
| `inproc` | `DEALER_ROUTER` | 55.4% | 58.4% | 58.9% | 20.3% | 50.6% | 81.3% | 미달 54.1%/lat1.64× · c0175-dotnet-single-inproc |
| `inproc` | `DEALER_ROUTER_REQREP` | 55.2% | 52.1% | 55.4% | 25.5% | 46.9% | 55.6% | 미달 48.4%/lat2.54× · c0175-dotnet-single-inproc |
| `inproc` | `ROUTER_ROUTER` | 54.4% | 58.5% | 58.7% | 26.2% | 52.4% | 73.9% | 미달 54.0%/lat1.55× · c0175-dotnet-single-inproc |
| `inproc` | `ROUTER_ROUTER_REQREP` | 46.3% | 49.3% | 45.8% | 7.4% | 14.6% | 22.0% | 미달 30.9%/lat23.52× · c0175-dotnet-single-inproc |
| `ipc` | `PAIR` | 37.8% | 45.3% | 62.1% | 86.7% | 91.7% | 89.1% | 미달 68.8%/lat0.95× · c0175-dotnet-single-ipc |
| `ipc` | `PUBSUB` | 45.8% | 49.6% | 74.3% | 152.0% | 170.7% | 163.6% | 통과 109.3%/lat1.29× · c0175-dotnet-single-ipc |
| `ipc` | `DEALER_DEALER` | 38.0% | 44.7% | 56.9% | 109.6% | 96.7% | 78.1% | 미달 70.7%/lat0.70× · c0175-dotnet-single-ipc |
| `ipc` | `DEALER_ROUTER` | 35.2% | 39.6% | 50.0% | 96.3% | 85.3% | 87.7% | 미달 65.7%/lat0.99× · c0175-dotnet-single-ipc |
| `ipc` | `DEALER_ROUTER_REQREP` | 45.6% | 43.0% | 42.0% | 59.1% | 77.1% | 78.0% | 미달 57.5%/lat1.48× · c0175-dotnet-single-ipc |
| `ipc` | `ROUTER_ROUTER` | 39.9% | 42.5% | 52.3% | 101.5% | 86.8% | 76.4% | 미달 66.6%/lat1.23× · c0175-dotnet-single-ipc |
| `ipc` | `ROUTER_ROUTER_REQREP` | 35.6% | 36.1% | 35.9% | 62.4% | 69.1% | 76.1% | 미달 52.5%/lat2.09× · c0175-dotnet-single-ipc |

#### 9.2.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 34.5% | 44.7% | 54.9% | 72.9% | 79.9% | 92.2% | 미달 63.2%/lat0.52× · c0175-dotnet-multi |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 47.8% | 43.3% | 47.0% | 57.8% | 80.7% | 57.4% | 미달 55.7%/lat2.50× · c0175-dotnet-multi |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 85.1% | 55.2% | 46.0% | 90.1% | 142.9% | 157.0% | 통과 96.0%/lat1.01× · c0175-dotnet-multi |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 101.2% | 82.7% | 83.7% | 85.9% | 99.0% | 44.9% | 통과 82.9%/lat0.92× · c0175-dotnet-multi |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 70.3% | 68.2% | 62.3% | 67.3% | 68.2% | 81.7% | 미달 69.7%/lat1.04× · c0175-dotnet-multi |
| `tcp` | `MULTI_PUBSUB` | 83.4% | 79.6% | 62.4% | 85.9% | 123.5% | 96.7% | 통과 88.6%/lat1.28× · c0175-dotnet-multi |
| `tcp` | `MULTI_STREAM` | 93.7% | 84.5% | 88.5% | 해당 없음 | 110.7% | 해당 없음 | 통과 94.3%/lat1.10× · c0175-dotnet-multi |
| `ws` | `MULTI_DEALER_DEALER` | 42.3% | 41.9% | 37.3% | 95.3% | 71.4% | 98.7% | 미달 64.5%/lat0.34× · c0175-dotnet-multi |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 66.5% | 71.2% | 54.3% | 130.8% | 79.3% | 106.8% | 통과 84.8%/lat0.95× · c0175-dotnet-multi |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 171.9% | 132.0% | 174.8% | 247.5% | 76.3% | 118.0% | 통과 153.4%/lat0.47× · c0175-dotnet-multi |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 72.8% | 64.7% | 75.3% | 70.3% | 92.8% | 117.1% | 통과 82.2%/lat1.02× · c0175-dotnet-multi |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 59.5% | 65.5% | 86.3% | 127.2% | 104.0% | 92.5% | 통과 89.2%/lat1.62× · c0175-dotnet-multi |
| `ws` | `MULTI_PUBSUB` | 45.8% | 111.6% | 168.0% | 172.9% | 302.2% | 131.1% | 통과 155.3%/lat0.89× · c0175-dotnet-multi |
| `ws` | `MULTI_STREAM` | 94.8% | 91.5% | 96.5% | 해당 없음 | 109.7% | 해당 없음 | 통과 98.1%/lat1.04× · c0175-dotnet-multi |
| `wss` | `MULTI_DEALER_DEALER` | 45.4% | 49.3% | 71.4% | 91.2% | 75.9% | 69.5% | 미달 67.1%/lat0.43× · c0175-dotnet-multi |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 64.7% | 58.7% | 85.0% | 116.2% | 107.6% | 83.0% | 통과 85.9%/lat0.60× · c0175-dotnet-multi |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 49.0% | 58.0% | 74.7% | 115.0% | 71.2% | 82.5% | 미달 75.1%/lat3.46× · c0175-dotnet-multi |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 73.1% | 65.3% | 60.9% | 206.6% | 115.1% | 104.7% | 통과 104.3%/lat0.71× · c0175-dotnet-multi |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 62.2% | 59.0% | 94.5% | 201.2% | 66.1% | 70.1% | 통과 92.2%/lat0.73× · c0175-dotnet-multi |
| `wss` | `MULTI_PUBSUB` | 35.3% | 40.5% | 61.1% | 95.3% | 111.0% | 107.8% | 미달 75.2%/lat1.09× · c0175-dotnet-multi |
| `wss` | `MULTI_STREAM` | 98.3% | 103.6% | 107.9% | 해당 없음 | 128.3% | 해당 없음 | 통과 109.5%/lat0.95× · c0175-dotnet-multi |
| `tls` | `MULTI_DEALER_DEALER` | 40.9% | 81.4% | 79.4% | 88.9% | 104.7% | 89.3% | 미달 80.8%/lat0.45× · c0175-dotnet-multi |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 50.6% | 52.7% | 62.0% | 159.9% | 107.6% | 111.6% | 통과 90.7%/lat0.68× · c0175-dotnet-multi |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 53.2% | 50.4% | 52.0% | 400.6% | 57.5% | 82.3% | 통과 116.0%/lat0.83× · c0175-dotnet-multi |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 88.4% | 89.8% | 100.7% | 160.4% | 167.5% | 129.9% | 통과 122.8%/lat0.73× · c0175-dotnet-multi |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 43.4% | 50.7% | 56.8% | 75.8% | 75.5% | 95.6% | 미달 66.3%/lat0.85× · c0175-dotnet-multi |
| `tls` | `MULTI_PUBSUB` | 55.1% | 68.3% | 81.0% | 135.8% | 118.6% | 149.1% | 통과 101.3%/lat0.91× · c0175-dotnet-multi |
| `tls` | `MULTI_STREAM` | 94.9% | 80.4% | 82.9% | 해당 없음 | 94.1% | 해당 없음 | 통과 88.1%/lat1.14× · c0175-dotnet-multi |

> **C multi 러너 AUTO_HWM_DETAIL 실패 — 해소됨(2026-09-09, 9f2a977d59).** 초기 0.17.5 측정에서
> `MULTI_ROUTER_ROUTER_SENDSEND` ws 65536·131072B의 **C multi 러너**가 `non_zero_exit_1`로 종료했고
> 첫 오류가 `AUTO_HWM_DETAIL`(monitor snapshot 조회) 단계였다. 원인은 Core가 아니라 **harness
> parity 결함**: `run_benchmarks.sh`(single)는 `--core-version`(release) 시 구형 monitor ABI를
> 피하려 `PERF_PRINT_AUTO_HWM_DETAIL=0`을 설정하는데 `run_benchmarks_multi.sh`에는 그 가드가 없었다.
> 동일 가드를 추가(측정 workload 불변, optional introspection만 skip)한 뒤 재측정에서 두 셀 모두
> `status: complete`로 통과. 이후 모든 언어 multi 측정은 이 수정본 러너로 진행한다.

### 9.3 Java

- perf 경로: `bindings/java/perf` (core **0.17.5**, JDK 22 필수: `JAVA_HOME=~/.jdks/jdk-22.0.2+9`, FFM API)
- Single 상태: `측정 완료(2026-09-09) + routed 개선 반영` — 6 transport × 7 pattern, 실패 0. 통과 24 / 미달 18.
  **completion drain inline 정착 최적화(306b939a98) 후 routed req/reply 재측정**: ws/wss/tls
  DEALER_ROUTER_REQREP 등 다수가 통과 전환(ws DEALER_ROUTER_REQREP 96.7%). tcp req/reply는 C tcp가 빨라
  여전히 미달(43%대). 남은 미달은 inproc·ipc(§2.1 Java 예외 없음) + tcp req/reply.
- Multi 상태: `측정 완료 + routed 개선 반영(2026-09-09)` — 4 transport × 7 pattern, clients=100, 실패 0.
  통과 6 / 미달 22. completion 최적화(306b939a98) 후 routed(SENDSEND·REQREP) 재측정으로 통과 2→6.
  routed req/reply·echo가 여전히 다수 미달(tcp req/reply가 특히 낮음) — 추가 개선 여지. C baseline은
  canonical C 0.17.5 재사용(C 재측정 없음).

> **Java routed 비율 심층 진단(2026-09-09, 프로파일 근거).** "req/reply가 async 세금으로 일률적으로 낮다"는 해석은
> 틀렸다. 프로파일이 밝힌 사실:
> - **REQREP 대형 붕괴는 Core 정책 절벽**이다. Core 0.17.5는 request 크기 B에 work-charge를 매기고(≤1KiB:B, 1~32KiB:
>   ceil(B³/1KiB²), >32KiB:32MiB+1; pair 예산 32MiB — `core/src/runtime/core/transport_pair_policy.hpp:14`,
>   `pipe_receive.cpp:624`, spec `06-auto-hwm.ko.md:629`), 그래서 **동시 미해결 request가 4KiB 512 → 16KiB 8 → 32KiB 1**로
>   급감한다. 32KiB부터 파이프라인이 1건으로 막혀 CPU가 놀며(32KiB: server 31%·client 57%, POLLOUT 99% 대기) throughput이
>   폭락한다(113→61→5 Kops/s). **C도 같은 절벽을 맞는다**(Core 계약). SENDSEND는 correlation 예약이 없어 절벽이 없다.
>   절벽에서 Java가 C보다 더 낮은 건 Java가 backpressure 뒤 payload를 보존·재제출(`CompletionOwner`)하는 반면 C는 사전
>   할당 slot flag/token만 갱신(`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:290`)하기 때문 — **분류 A(문서화된
>   Core 계약에 대한 Java 재제출 경로 적응)**. 단 절대 처리량은 Core가 캡하므로 절벽 자체는 못 넘는다.
> - **SENDSEND 소형 저성능은 양쪽 CPU 포화**다(64B: server 364%·client 277%). Java 벤치 relay가 매 메시지 routing-id 방어
>   복사 + payload 깊은 복사를 하는데 C relay는 `zlink_msg_move`로 소유권만 옮긴다(`perf_multi_relay_server.hpp:120`).
>   즉 고정 per-message 비용(객체·FFM·ownership) + **Java harness relay의 copy↔C의 move 비대칭**이다. payload가 커지면 희석돼
>   비율이 회복한다. 이 harness 비대칭은 parity로 정렬 검토 대상(수치만 올리는 게 아니라 동일 의미 정렬).

#### 9.3.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 69.9% | 71.4% | 108.8% | 170.4% | 139.7% | 107.2% | 통과 111.2%/lat1.00× · c0175-java-single |
| `tcp` | `PUBSUB` | 64.8% | 71.6% | 99.3% | 99.4% | 100.2% | 102.3% | 미달 89.6%/lat1.36× · c0175-java-single |
| `tcp` | `DEALER_DEALER` | 77.4% | 78.4% | 97.5% | 160.5% | 134.1% | 111.5% | 통과 109.9%/lat1.02× · c0175-java-single |
| `tcp` | `DEALER_ROUTER` | 77.9% | 87.3% | 108.5% | 157.1% | 141.1% | 115.3% | 통과 114.5%/lat1.04× · c0175-java-single |
| `tcp` | `DEALER_ROUTER_REQREP` | 41.5% | 38.4% | 52.9% | 35.7% | 41.6% | 51.3% | 미달 43.6%/lat2.11× · c0175-java-single |
| `tcp` | `ROUTER_ROUTER` | 72.6% | 81.9% | 108.9% | 157.9% | 161.7% | 121.7% | 통과 117.5%/lat0.67× · c0175-java-single |
| `tcp` | `ROUTER_ROUTER_REQREP` | 42.9% | 42.8% | 59.3% | 24.6% | 36.9% | 40.6% | 미달 41.2%/lat3.35× · c0175-java-single |
| `ws` | `PAIR` | 66.3% | 75.9% | 106.6% | 147.9% | 122.2% | 109.8% | 통과 104.8%/lat0.14× · c0175-java-single |
| `ws` | `PUBSUB` | 72.4% | 61.7% | 83.9% | 100.4% | 102.1% | 102.8% | 미달 87.2%/lat1.02× · c0175-java-single |
| `ws` | `DEALER_DEALER` | 73.5% | 74.7% | 108.7% | 133.3% | 130.1% | 111.4% | 통과 105.3%/lat0.10× · c0175-java-single |
| `ws` | `DEALER_ROUTER` | 81.8% | 79.1% | 112.2% | 127.7% | 128.4% | 112.3% | 통과 106.9%/lat0.10× · c0175-java-single |
| `ws` | `DEALER_ROUTER_REQREP` | 92.7% | 112.6% | 95.4% | 80.3% | 86.4% | 112.9% | 통과 96.7%/lat0.94× · c0175-java-single |
| `ws` | `ROUTER_ROUTER` | 89.4% | 78.8% | 120.4% | 154.5% | 152.6% | 142.1% | 통과 123.0%/lat0.06× · c0175-java-single |
| `ws` | `ROUTER_ROUTER_REQREP` | 65.5% | 87.5% | 68.6% | 40.1% | 45.9% | 56.0% | 미달 60.6%/lat1.88× · c0175-java-single |
| `wss` | `PAIR` | 71.5% | 72.3% | 122.6% | 130.7% | 135.3% | 134.5% | 통과 111.1%/lat0.14× · c0175-java-single |
| `wss` | `PUBSUB` | 69.3% | 75.0% | 145.7% | 116.5% | 99.4% | 99.4% | 통과 100.9%/lat0.07× · c0175-java-single |
| `wss` | `DEALER_DEALER` | 82.0% | 88.2% | 107.4% | 134.1% | 137.0% | 122.8% | 통과 111.9%/lat0.11× · c0175-java-single |
| `wss` | `DEALER_ROUTER` | 78.1% | 91.2% | 134.9% | 145.1% | 146.6% | 136.9% | 통과 122.1%/lat0.12× · c0175-java-single |
| `wss` | `DEALER_ROUTER_REQREP` | 75.2% | 135.8% | 170.9% | 104.0% | 193.6% | 229.1% | 통과 151.4%/lat0.47× · c0175-java-single |
| `wss` | `ROUTER_ROUTER` | 89.0% | 108.3% | 162.6% | 169.5% | 185.4% | 177.5% | 통과 148.7%/lat0.07× · c0175-java-single |
| `wss` | `ROUTER_ROUTER_REQREP` | 62.2% | 160.4% | 168.7% | 62.6% | 76.6% | 96.1% | 통과 104.4%/lat1.07× · c0175-java-single |
| `tls` | `PAIR` | 65.9% | 77.4% | 177.4% | 148.3% | 154.3% | 153.3% | 미달 129.4%/lat4.42× · c0175-java-single |
| `tls` | `PUBSUB` | 63.4% | 89.4% | 195.0% | 105.0% | 98.5% | 101.6% | 통과 108.8%/lat0.86× · c0175-java-single |
| `tls` | `DEALER_DEALER` | 79.1% | 78.0% | 153.3% | 155.0% | 158.7% | 160.4% | 통과 130.8%/lat2.42× · c0175-java-single |
| `tls` | `DEALER_ROUTER` | 74.8% | 89.9% | 160.8% | 152.4% | 147.3% | 150.3% | 통과 129.2%/lat2.55× · c0175-java-single |
| `tls` | `DEALER_ROUTER_REQREP` | 58.8% | 45.2% | 124.7% | 60.5% | 76.4% | 88.8% | 통과 75.7%/lat1.18× · c0175-java-single |
| `tls` | `ROUTER_ROUTER` | 88.4% | 100.1% | 152.5% | 175.7% | 181.7% | 164.9% | 통과 143.9%/lat0.35× · c0175-java-single |
| `tls` | `ROUTER_ROUTER_REQREP` | 54.5% | 52.4% | 125.9% | 50.6% | 67.2% | 80.4% | 통과 71.8%/lat1.33× · c0175-java-single |
| `inproc` | `PAIR` | 71.2% | 75.8% | 80.8% | 103.4% | 133.8% | 85.7% | 통과 91.8%/lat1.58× · c0175-java-single |
| `inproc` | `PUBSUB` | 76.9% | 73.8% | 76.3% | 14.8% | 10.3% | 13.2% | 미달 44.2%/lat4.81× · c0175-java-single |
| `inproc` | `DEALER_DEALER` | 70.5% | 75.0% | 68.4% | 42.8% | 38.8% | 43.7% | 미달 56.5%/lat1.84× · c0175-java-single |
| `inproc` | `DEALER_ROUTER` | 59.9% | 68.2% | 59.4% | 36.8% | 39.2% | 39.7% | 미달 50.5%/lat2.03× · c0175-java-single |
| `inproc` | `DEALER_ROUTER_REQREP` | 72.4% | 74.4% | 54.4% | 34.9% | 36.2% | 36.7% | 미달 51.5%/lat3.70× · c0175-java-single |
| `inproc` | `ROUTER_ROUTER` | 81.2% | 82.2% | 85.7% | 130.0% | 147.4% | 127.5% | 통과 109.0%/lat2.90× · c0175-java-single |
| `inproc` | `ROUTER_ROUTER_REQREP` | 51.1% | 42.9% | 42.5% | 21.1% | 24.2% | 20.7% | 미달 33.8%/lat6.75× · c0175-java-single |
| `ipc` | `PAIR` | 70.6% | 75.3% | 93.9% | 79.1% | 80.4% | 66.5% | 미달 77.6%/lat1.44× · c0175-java-single |
| `ipc` | `PUBSUB` | 65.1% | 69.0% | 98.4% | 100.4% | 101.9% | 102.9% | 미달 89.6%/lat1.28× · c0175-java-single |
| `ipc` | `DEALER_DEALER` | 76.2% | 75.9% | 89.8% | 111.8% | 82.1% | 66.6% | 미달 83.7%/lat1.34× · c0175-java-single |
| `ipc` | `DEALER_ROUTER` | 66.8% | 73.1% | 84.5% | 97.4% | 75.8% | 58.0% | 미달 75.9%/lat1.27× · c0175-java-single |
| `ipc` | `DEALER_ROUTER_REQREP` | 64.5% | 55.4% | 52.3% | 52.3% | 60.4% | 56.6% | 미달 56.9%/lat2.00× · c0175-java-single |
| `ipc` | `ROUTER_ROUTER` | 72.8% | 75.6% | 73.3% | 99.0% | 73.1% | 59.8% | 미달 75.6%/lat1.40× · c0175-java-single |
| `ipc` | `ROUTER_ROUTER_REQREP` | 51.9% | 51.2% | 45.5% | 36.3% | 39.5% | 46.5% | 미달 45.1%/lat2.94× · c0175-java-single |

#### 9.3.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 64.4% | 66.2% | 69.6% | 64.3% | 40.4% | 38.2% | 미달 57.2%/lat0.29× · c0175-java-multi |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 50.6% | 51.0% | 45.8% | 30.0% | 52.4% | 55.7% | 미달 47.6%/lat241.14× · c0175-java-multi |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 63.8% | 69.6% | 74.2% | 72.2% | 15.9% | 20.5% | 미달 52.7%/lat1.50× · c0175-java-multi |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 63.7% | 108.8% | 106.7% | 47.6% | 71.2% | 60.9% | 통과 76.5%/lat2.86× · c0175-java-multi |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 89.7% | 86.2% | 96.5% | 96.8% | 22.7% | 28.1% | 통과 70.0%/lat1.27× · c0175-java-multi |
| `tcp` | `MULTI_PUBSUB` | 57.3% | 67.1% | 71.8% | 81.4% | 120.5% | 156.1% | 통과 92.4%/lat0.93× · c0175-java-multi |
| `tcp` | `MULTI_STREAM` | 62.0% | 77.9% | 82.5% | 해당 없음 | 104.1% | 해당 없음 | 미달 81.6%/lat1.25× · c0175-java-multi |
| `ws` | `MULTI_DEALER_DEALER` | 84.7% | 74.8% | 72.3% | 110.0% | 48.0% | 42.9% | 미달 72.1%/lat0.24× · c0175-java-multi |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 54.2% | 65.9% | 67.2% | 66.6% | 98.9% | 163.8% | 미달 86.1%/lat16.22× · c0175-java-multi |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 74.1% | 86.3% | 79.5% | 161.9% | 35.5% | 38.2% | 미달 79.2%/lat4.25× · c0175-java-multi |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 97.0% | 77.8% | 47.4% | 19.3% | 47.7% | 80.1% | 미달 61.6%/lat4.22× · c0175-java-multi |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 91.6% | 91.2% | 114.4% | 152.7% | 38.7% | 39.2% | 통과 88.0%/lat2.96× · c0175-java-multi |
| `ws` | `MULTI_PUBSUB` | 64.0% | 63.8% | 69.2% | 69.6% | 136.2% | 114.0% | 미달 86.1%/lat0.90× · c0175-java-multi |
| `ws` | `MULTI_STREAM` | 86.7% | 77.8% | 73.1% | 해당 없음 | 85.8% | 해당 없음 | 미달 80.8%/lat1.23× · c0175-java-multi |
| `wss` | `MULTI_DEALER_DEALER` | 59.9% | 64.5% | 78.4% | 55.1% | 58.4% | 73.6% | 미달 65.0%/lat0.29× · c0175-java-multi |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 89.4% | 100.5% | 99.2% | 80.9% | 62.5% | 54.3% | 통과 81.1%/lat1.23× · c0175-java-multi |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 76.7% | 70.0% | 98.2% | 57.8% | 38.7% | 54.1% | 미달 65.9%/lat2.48× · c0175-java-multi |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 51.5% | 60.0% | 73.0% | 110.9% | 50.6% | 56.7% | 미달 67.1%/lat1.21× · c0175-java-multi |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 58.0% | 57.5% | 66.3% | 94.0% | 32.2% | 41.1% | 미달 58.2%/lat3.21× · c0175-java-multi |
| `wss` | `MULTI_PUBSUB` | 39.2% | 49.6% | 58.3% | 75.7% | 81.6% | 88.9% | 미달 65.5%/lat1.04× · c0175-java-multi |
| `wss` | `MULTI_STREAM` | 59.8% | 70.0% | 67.1% | 해당 없음 | 80.5% | 해당 없음 | 미달 69.3%/lat1.46× · c0175-java-multi |
| `tls` | `MULTI_DEALER_DEALER` | 66.3% | 79.9% | 101.1% | 50.0% | 69.5% | 56.6% | 미달 70.6%/lat0.44× · c0175-java-multi |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 69.3% | 73.2% | 68.7% | 47.7% | 47.3% | 54.1% | 미달 60.1%/lat3.96× · c0175-java-multi |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 69.8% | 73.0% | 74.0% | 113.1% | 27.5% | 38.3% | 미달 66.0%/lat1.22× · c0175-java-multi |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 67.3% | 75.0% | 67.7% | 110.0% | 54.4% | 57.9% | 통과 72.0%/lat1.38× · c0175-java-multi |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 51.1% | 70.3% | 69.8% | 80.2% | 36.0% | 59.2% | 미달 61.1%/lat1.15× · c0175-java-multi |
| `tls` | `MULTI_PUBSUB` | 64.8% | 54.9% | 77.6% | 92.9% | 102.2% | 106.4% | 미달 83.1%/lat1.01× · c0175-java-multi |
| `tls` | `MULTI_STREAM` | 52.6% | 58.4% | 59.8% | 해당 없음 | 72.7% | 해당 없음 | 미달 60.9%/lat1.69× · c0175-java-multi |

### 9.4 Node

- perf 경로: `bindings/node/perf` (core **0.17.5**, Node 22+ 필수: `~/.cache/zlink/node-v22.23.2-linux-x64`)
- Single 상태: `측정 완료(2026-09-09)` — 통과 16 / 미달 19 / 해당없음 7. **inproc는 바인딩 미지원**
  ("inproc context is worker-local", Node worker-thread 구조 제약 → 해당 없음, 수정 대상 아님).
  나머지 미달은 작은 payload와 req/reply. C baseline은 canonical 0.17.5 재사용(C 재측정 안 함).
- Multi 상태: `측정 완료(2026-09-09, 실패 수정 반영)` — 5 pattern(REQREP 계열은 Node 러너 미등록 → 표
  제외) × 4 transport. 통과 3 / 미달 17 / 미측정 0. 최초 7셀(ws·wss·tls MULTI_DEALER_ROUTER_SENDSEND
  작은 size)이 `send admission / echo drain timed out`으로 실패했으나 **원인이 Node perf harness의 커스텀
  응답 큐가 비동기 admission을 잘못 관리한 것**임을 확인해 네이티브 reply API로 수정(802f1efab9, 회귀 테스트
  포함) 후 재측정으로 해소(ws 39.6·wss 42.8·tls 39.3%). Core·바인딩 라이브러리 무변경. STREAM은 최초 job이
  비-STREAM partial 때문에 건너뛴 것을 재측정해 채움. routed echo·req/reply는 30~40%대로 목표 미달(2단계).
- 다음 작업: 실패 셀 원인 진단·수정 후 재측정(§9.4 하단 메모).

> **Node routed echo(SENDSEND) 개선 시도 — 보류(2026-09-09).** MULTI_DEALER_ROUTER_SENDSEND 등이 C 대비 매우 낮아
> (tcp 24.9%) 프로파일했다. echo당 최소 recv 1회·submit 1회의 **JS↔native 경계가 CPU의 ~78%**(submitSend 41.7% + routed
> recv 36.5%)를 차지하고 GC는 1.98%뿐이다. Java의 completion worker-queue 왕복 문제는 없었다(그 실험은 효과 없어 revert).
> 근본 병목은 SENDSEND의 2-part(payload+빈 tail) multipart 바인딩 경계 비용이다(1-part 진단은 +65% 빨랐다). 권장 수정은
> routed multipart part를 기존 `nativeReadOnly` 저장으로 materialize해 복사를 줄이는 것이나, 이는 **protected spec
> `bindings/doc/spec/node/README.ko.md`의 routed lazy-materialization 예외와 비용 계약을 바꿔야** 가능하다. 계약/스펙 변경은
> perf를 위해 우회하지 않으므로(§7.5·§8) **보류**로 둔다. 코드·테스트 무변경(routed contract test 12/12 통과). 스펙 개정을
> 승인하면 별도 설계로 다룬다.

#### 9.4.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 23.5% | 26.5% | 35.2% | 160.9% | 125.4% | 106.2% | 미달 79.6%/lat20.04× · c0175-node-single |
| `tcp` | `PUBSUB` | 22.7% | 23.7% | 31.9% | 100.9% | 152.3% | 95.5% | 미달 71.2%/lat39.66× · c0175-node-single |
| `tcp` | `DEALER_DEALER` | 27.0% | 26.1% | 30.2% | 163.4% | 128.5% | 104.5% | 통과 80.0%/lat2.19× · c0175-node-single |
| `tcp` | `DEALER_ROUTER` | 17.0% | 19.5% | 24.6% | 140.4% | 111.3% | 85.9% | 미달 66.5%/lat33.48× · c0175-node-single |
| `tcp` | `DEALER_ROUTER_REQREP` | 18.0% | 15.7% | 19.3% | 45.8% | 47.1% | 46.8% | 미달 32.1%/lat4.01× · c0175-node-single |
| `tcp` | `ROUTER_ROUTER` | 20.9% | 25.8% | 31.3% | 184.1% | 174.3% | 124.3% | 통과 93.5%/lat2.50× · c0175-node-single |
| `tcp` | `ROUTER_ROUTER_REQREP` | 15.7% | 15.2% | 19.7% | 37.8% | 43.1% | 39.5% | 미달 28.5%/lat6.04× · c0175-node-single |
| `ws` | `PAIR` | 23.0% | 25.7% | 31.5% | 162.6% | 137.4% | 119.9% | 통과 83.4%/lat2.23× · c0175-node-single |
| `ws` | `PUBSUB` | 22.7% | 22.1% | 27.6% | 101.3% | 92.6% | 132.7% | 통과 66.5%/lat4.26× · c0175-node-single |
| `ws` | `DEALER_DEALER` | 25.9% | 24.1% | 30.0% | 154.1% | 139.8% | 113.2% | 통과 81.2%/lat1.76× · c0175-node-single |
| `ws` | `DEALER_ROUTER` | 18.1% | 18.9% | 22.6% | 133.0% | 127.4% | 108.5% | 통과 71.4%/lat3.05× · c0175-node-single |
| `ws` | `DEALER_ROUTER_REQREP` | 28.6% | 37.0% | 27.2% | 83.3% | 68.1% | 69.4% | 미달 52.3%/lat1.78× · c0175-node-single |
| `ws` | `ROUTER_ROUTER` | 21.6% | 23.1% | 27.4% | 152.0% | 141.9% | 129.1% | 통과 82.5%/lat2.69× · c0175-node-single |
| `ws` | `ROUTER_ROUTER_REQREP` | 19.9% | 31.0% | 19.8% | 37.2% | 39.2% | 40.2% | 미달 31.2%/lat2.62× · c0175-node-single |
| `wss` | `PAIR` | 23.7% | 28.7% | 48.6% | 151.1% | 146.5% | 138.1% | 통과 89.5%/lat1.61× · c0175-node-single |
| `wss` | `PUBSUB` | 21.6% | 24.2% | 47.0% | 109.1% | 91.8% | 84.2% | 통과 63.0%/lat1.77× · c0175-node-single |
| `wss` | `DEALER_DEALER` | 25.4% | 29.4% | 49.4% | 153.7% | 145.7% | 128.0% | 통과 88.6%/lat1.52× · c0175-node-single |
| `wss` | `DEALER_ROUTER` | 17.5% | 22.3% | 42.0% | 174.6% | 160.2% | 150.9% | 통과 94.6%/lat1.88× · c0175-node-single |
| `wss` | `DEALER_ROUTER_REQREP` | 25.4% | 41.6% | 58.4% | 119.7% | 229.2% | 240.7% | 통과 119.2%/lat0.60× · c0175-node-single |
| `wss` | `ROUTER_ROUTER` | 22.1% | 29.4% | 56.9% | 235.7% | 223.8% | 192.2% | 통과 126.7%/lat1.31× · c0175-node-single |
| `wss` | `ROUTER_ROUTER_REQREP` | 17.1% | 39.5% | 43.5% | 61.3% | 73.7% | 87.9% | 미달 53.8%/lat1.21× · c0175-node-single |
| `tls` | `PAIR` | 22.0% | 28.9% | 74.9% | 178.8% | 170.2% | 142.5% | 미달 102.9%/lat14.27× · c0175-node-single |
| `tls` | `PUBSUB` | 20.5% | 29.6% | 68.9% | 97.3% | 98.4% | 94.5% | 미달 68.2%/lat13.04× · c0175-node-single |
| `tls` | `DEALER_DEALER` | 25.6% | 25.5% | 58.6% | 163.2% | 165.3% | 156.6% | 미달 99.1%/lat7.00× · c0175-node-single |
| `tls` | `DEALER_ROUTER` | 16.7% | 21.5% | 54.6% | 186.6% | 167.8% | 156.1% | 미달 100.5%/lat18.20× · c0175-node-single |
| `tls` | `DEALER_ROUTER_REQREP` | 20.5% | 20.3% | 46.7% | 77.1% | 87.1% | 90.1% | 미달 57.0%/lat1.27× · c0175-node-single |
| `tls` | `ROUTER_ROUTER` | 22.5% | 31.6% | 52.4% | 215.8% | 207.7% | 172.5% | 통과 117.1%/lat1.51× · c0175-node-single |
| `tls` | `ROUTER_ROUTER_REQREP` | 16.8% | 18.3% | 43.1% | 55.4% | 73.6% | 82.3% | 미달 48.2%/lat1.45× · c0175-node-single |
| `inproc` | `PAIR` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음(바인딩 미지원) |
| `inproc` | `PUBSUB` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음(바인딩 미지원) |
| `inproc` | `DEALER_DEALER` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음(바인딩 미지원) |
| `inproc` | `DEALER_ROUTER` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음(바인딩 미지원) |
| `inproc` | `DEALER_ROUTER_REQREP` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음(바인딩 미지원) |
| `inproc` | `ROUTER_ROUTER` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음(바인딩 미지원) |
| `inproc` | `ROUTER_ROUTER_REQREP` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음(바인딩 미지원) |
| `ipc` | `PAIR` | 23.7% | 25.3% | 28.9% | 128.9% | 108.0% | 84.0% | 미달 66.5%/lat9.82× · c0175-node-single |
| `ipc` | `PUBSUB` | 24.1% | 24.8% | 31.7% | 102.4% | 99.8% | 99.8% | 미달 63.8%/lat15.13× · c0175-node-single |
| `ipc` | `DEALER_DEALER` | 25.7% | 26.6% | 27.9% | 124.9% | 109.6% | 86.4% | 통과 66.9%/lat1.77× · c0175-node-single |
| `ipc` | `DEALER_ROUTER` | 17.7% | 19.4% | 22.7% | 141.2% | 119.7% | 96.5% | 통과 69.5%/lat4.55× · c0175-node-single |
| `ipc` | `DEALER_ROUTER_REQREP` | 20.0% | 17.4% | 17.2% | 64.4% | 64.7% | 50.8% | 미달 39.1%/lat5.29× · c0175-node-single |
| `ipc` | `ROUTER_ROUTER` | 20.8% | 22.2% | 23.3% | 143.1% | 111.9% | 86.2% | 미달 67.9%/lat9.77× · c0175-node-single |
| `ipc` | `ROUTER_ROUTER_REQREP` | 14.7% | 14.2% | 13.3% | 32.6% | 38.0% | 39.6% | 미달 25.4%/lat7.68× · c0175-node-single |

#### 9.4.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 24.1% | 25.7% | 27.9% | 56.0% | 67.3% | 70.2% | 미달 45.2%/lat131.96× · c0175-node-multi |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 8.6% | 14.3% | 13.9% | 16.5% | 38.8% | 22.2% | 미달 19.1%/lat548.91× · c0175-node-multi |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 18.3% | 23.5% | 18.6% | 19.2% | 44.1% | 41.5% | 미달 27.5%/lat551.52× · c0175-node-multi |
| `tcp` | `MULTI_PUBSUB` | 21.4% | 20.1% | 19.1% | 24.5% | 58.4% | 80.5% | 미달 37.3%/lat0.70× · c0175-node-multi |
| `tcp` | `MULTI_STREAM` | 42.84% | 49.11% | 42.18% | 해당 없음 | 84.61% | 해당 없음 | 미달 54.7%/lat2.19× · c0175-node-multi |
| `ws` | `MULTI_DEALER_DEALER` | 26.7% | 25.5% | 28.5% | 66.7% | 83.3% | 69.7% | 미달 50.1%/lat1.11× · c0175-node-multi |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 29.3% | 32.8% | 30.8% | 24.7% | 48.9% | 70.9% | 미달 39.6%/lat61.04× · c0175-node-multi |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 25.1% | 28.6% | 29.0% | 29.4% | 44.4% | 56.8% | 미달 35.6%/lat37.61× · c0175-node-multi |
| `ws` | `MULTI_PUBSUB` | 23.7% | 24.3% | 21.8% | 20.3% | 93.4% | 102.9% | 미달 47.7%/lat0.49× · c0175-node-multi |
| `ws` | `MULTI_STREAM` | 55.13% | 52.49% | 38.12% | 해당 없음 | 118.08% | 해당 없음 | 통과 66.0%/lat1.86× · c0175-node-multi |
| `wss` | `MULTI_DEALER_DEALER` | 22.6% | 25.0% | 35.8% | 61.4% | 56.8% | 61.6% | 미달 43.9%/lat35.05× · c0175-node-multi |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 28.6% | 31.2% | 19.6% | 43.0% | 70.7% | 63.8% | 미달 42.8%/lat2.23× · c0175-node-multi |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 17.0% | 22.0% | 25.5% | 37.0% | 26.9% | 29.9% | 미달 26.4%/lat4.67× · c0175-node-multi |
| `wss` | `MULTI_PUBSUB` | 19.8% | 23.3% | 20.9% | 40.3% | 70.6% | 73.0% | 미달 41.3%/lat0.43× · c0175-node-multi |
| `wss` | `MULTI_STREAM` | 74.27% | 65.85% | 41.67% | 해당 없음 | 117.30% | 해당 없음 | 통과 74.8%/lat1.43× · c0175-node-multi |
| `tls` | `MULTI_DEALER_DEALER` | 21.9% | 22.0% | 34.5% | 44.0% | 56.3% | 47.9% | 미달 37.8%/lat10.28× · c0175-node-multi |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 29.6% | 28.4% | 13.9% | 27.6% | 68.8% | 67.4% | 미달 39.3%/lat120.09× · c0175-node-multi |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 22.4% | 21.9% | 10.5% | 24.7% | 25.2% | 32.0% | 미달 22.8%/lat126.77× · c0175-node-multi |
| `tls` | `MULTI_PUBSUB` | 23.1% | 20.9% | 23.1% | 45.3% | 67.5% | 65.9% | 미달 41.0%/lat0.52× · c0175-node-multi |
| `tls` | `MULTI_STREAM` | 63.67% | 44.26% | 39.55% | 해당 없음 | 98.45% | 해당 없음 | 통과 61.5%/lat1.92× · c0175-node-multi |

> **Node multi 실패 — 해소됨(2026-09-09, 802f1efab9).** `MULTI_DEALER_ROUTER_SENDSEND`의 작은 payload
> 7셀(ws 64·256·1024B, wss 64·256B, tls 64·256B, 100 CCU)이 `send admission / echo drain timed out
> (echoes=…, admissions=0)`으로 실패했다. **근본 원인은 Core·바인딩이 아니라 Node perf harness**: 서버
> 응답을 커스텀 `RoutedReplySender` 비동기 큐로 보내다 고메시지율에서 대기 응답이 다음 public submit을
> gate해 send-admission이 진행되지 못했다. 서버 응답을 수신 메시지의 네이티브 reply 빌더
> (`received.send().message(part).submit()`)로 바꾸고 커스텀 큐(~100줄)를 제거해 해소(회귀 테스트 추가).
> timeout·HWM·client 수 불변(우회 아님). 재측정 결과 ws 39.6·wss 42.8·tls 39.3%(throughput은 목표 미달).

### 9.5 Go

- perf 경로: `bindings/go/perf`
- Single 상태: `미측정`
- Multi 상태: `미측정`
- 다음 작업: 현재 binding runner에 등록된 pattern을 inventory gate에서 확인한 뒤 paired 측정을 시작한다.

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

#### 9.6.2 Multi suite

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
| 2026-09-09 | 전체 | 계획 초기화 | - | Core 0.17.4 release, C 기준과 binding paired 비교, 단일 perf process 조건을 사용한다. | 계획 작성 | 이 문서 |
| 2026-09-10 | cpp | multi routed | c0175-cpp-multi-rebaseline | **Core 0.17.5**(cpp도 최신으로 통일), relay 정리 후, tcp, runs=3 median, C 0.17.5 baseline 재사용 | 아래 §11.1 | `results-cpp-multi-rebaseline.tsv` (10e06c7988) |
| 2026-09-10 | node | multi routed | c0175-node-multi-rebaseline | Core 0.17.5, relay 정리 후, tcp, runs=3 median | 아래 §11.1 | `results-node-multi-rebaseline.tsv` (4ad7a50a82) |

### 11.1 Core 0.17.5 routed 재측정 (relay 정리 후, tcp, runs=3, C 0.17.5 대비 %)

relay anti-pattern 정리 후 최신 Core 0.17.5로 4개 언어 routed 4패턴을 재측정한다(cpp도 0.17.4→0.17.5). "최악 = 목표 median 대비 pattern
평균 갭 최대"로 개선 대상을 정한다. **목표 median: cpp routed/reqrep=85, node routed/reqrep=60**(§ autofill TARGETS).

**cpp** (0.17.5, tcp %C / pattern 평균 → 목표85 갭):

| pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 평균 | 갭(85−) |
|---|--|--|--|--|--|--|--|--|
| MULTI_DEALER_ROUTER_SENDSEND | 92.8 | 82.5 | 78.4 | 89.0 | 78.2 | 100.9 | 87.0 | -2.0 (통과) |
| MULTI_ROUTER_ROUTER_SENDSEND | 66.3 | 102.1 | 92.7 | 94.9 | 92.3 | **47.0** | 82.6 | +2.4 (경미) |
| MULTI_DEALER_ROUTER_REQREP | 77.0 | 81.1 | 83.4 | 90.7 | 86.2 | 99.0 | 86.2 | -1.2 (통과) |
| MULTI_ROUTER_ROUTER_REQREP | 94.4 | 79.9 | 86.2 | 87.9 | 94.0 | 104.4 | 91.1 | -6.1 (통과) |

> cpp는 pattern 평균으로는 사실상 목표 충족. 단 개별 셀 `RR_SENDSEND 131072=47.0`·`64=66.3`가 outlier(대형은 노이즈 가능성, 확인 필요).

**node** (0.17.5, tcp %C / pattern 평균 → 목표60 갭):

| pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 평균 | 갭(60−) |
|---|--|--|--|--|--|--|--|--|
| MULTI_DEALER_ROUTER_SENDSEND | 24.2 | 20.9 | 18.7 | 20.9 | 59.0 | 70.7 | 35.7 | **24.3** |
| MULTI_ROUTER_ROUTER_SENDSEND | 27.8 | 37.9 | 32.9 | 38.0 | 48.4 | 42.3 | 37.9 | 22.1 |
| MULTI_DEALER_ROUTER_REQREP | 22.3 | 28.5 | 32.9 | 33.8 | 42.2 | 47.8 | 34.6 | **25.4** |
| MULTI_ROUTER_ROUTER_REQREP | 40.0 | 40.8 | 42.0 | 40.5 | 56.9 | 56.3 | 46.1 | 13.9 |

> node는 전 pattern이 목표 median 60에 미달. 특히 **DEALER_ROUTER (REQREP 갭 25.4, SENDSEND 24.3)** 의 작은 size(64~4096 ~19~34%)가 최저.
> node의 작은 size routed가 구조적 병목(N-API 경계·per-message 오버헤드) 후보.

**java** (0.17.5, tcp %C / pattern 평균 → 목표 routed85·reqrep70):

| pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 평균 | 판정 |
|---|--|--|--|--|--|--|--|--|
| MULTI_DEALER_ROUTER_SENDSEND | 94.0 | 102.5 | 72.8 | 76.7 | 86.8 | 99.5 | 88.7 | 통과 |
| MULTI_ROUTER_ROUTER_SENDSEND | 99.2 | 159.7 | 149.0 | 67.0 | 61.0 | 81.3 | 102.9 | 통과 |
| MULTI_DEALER_ROUTER_REQREP | 84.6 | 107.4 | 115.4 | 116.2 | **27.0** | **37.7** | 81.4 | 통과(대용량 셀 급락) |
| MULTI_ROUTER_ROUTER_REQREP | 107.0 | 123.1 | 131.2 | 128.3 | **28.4** | **39.5** | 92.9 | 통과(대용량 셀 급락) |

> java는 relay 정리로 routed 평균이 이전 70.5%→81~103%로 크게 개선(목표 충족). 단 **REQREP 대용량(65536/131072)=27~40%** 는 기존 Core
> work-charge cubic 절벽 + 바인딩 async 상호작용(§ 별도 기록). Core 레벨 요인이라 바인딩 단독 개선 대상 아님.

**dotnet** (0.17.5, tcp %C / pattern 평균 → 목표 routed80·reqrep70):

| pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 평균 | 갭 |
|---|--|--|--|--|--|--|--|--|
| MULTI_DEALER_ROUTER_SENDSEND | 49.4 | 58.5 | 52.2 | 71.0 | 81.2 | 60.5 | 62.1 | **17.9(미달)** |
| MULTI_ROUTER_ROUTER_SENDSEND | 53.7 | 70.3 | 70.3 | 70.5 | 112.3 | 39.5 | 69.4 | 10.6(미달) |
| MULTI_DEALER_ROUTER_REQREP | 56.9 | 64.6 | 66.2 | 70.4 | 64.2 | 78.0 | 66.7 | 3.3(경미) |
| MULTI_ROUTER_ROUTER_REQREP | 72.7 | 67.0 | 66.8 | 71.8 | 76.7 | 91.4 | 74.4 | 통과 |

### 11.2 목표 대비 갭 순위 → 개선 대상

pattern 평균의 목표 median 대비 갭(큰 순):

| 순위 | 언어·pattern | 평균%C | 목표 | 갭 |
|--|--|--|--|--|
| 1 | **node · DEALER_ROUTER_REQREP** | 34.6 | 60 | **25.4** |
| 2 | node · DEALER_ROUTER_SENDSEND | 35.7 | 60 | 24.3 |
| 3 | node · ROUTER_ROUTER_SENDSEND | 37.9 | 60 | 22.1 |
| 4 | dotnet · DEALER_ROUTER_SENDSEND | 62.1 | 80 | 17.9 |
| 5 | node · ROUTER_ROUTER_REQREP | 46.1 | 60 | 13.9 |
| 6 | dotnet · ROUTER_ROUTER_SENDSEND | 69.4 | 80 | 10.6 |

- cpp·java는 pattern 평균으로 목표 충족(각각 outlier 셀: cpp `RR_SENDSEND 131072=47`, java 대용량 REQREP는 Core 절벽 요인).
- **셀 단위 최악**: node 작은 size routed(예: `DEALER_ROUTER_SENDSEND 1024=18.7` → 갭 41). node routed hot-path가 전 pattern에 걸쳐
  목표 미달이라 **구조적(systemic) 병목**으로 판단.
- **개선 1순위 = Node routed hot-path**(N-API 경계·per-message wrapper·event-loop 오버헤드). 근본원인 진단 → 최적화 → §7.7 채택/revert.
  이후 dotnet DEALER_ROUTER_SENDSEND(2순위)로 이어간다.

### 11.3 개선 #1 — Node routed relay native frame 보존 (채택, commit f317aa4d7b)

- **근본원인(프로파일 근거):** Node routed relay가 record당 **수신 payload를 JS Buffer로 복사한 뒤 send용 native message를 재생성**(+ send-side
  snapshot 객체 2개/record + property lookup)해, C에 없는 per-message 고정비가 작은 size를 지배했다. `submitSend`가 server CPU의 44.9%.
- **최적화:** 관찰(read)되지 않은 routed multipart payload를 **receive→submit 내내 native frame으로 보존**(eager Buffer 복사·part별 snapshot 제거),
  .NET/C++처럼 native 소유권을 직접 전달. addon(`addon_core.cc`)+`message_snapshot.ts`+`message_conversion.ts`. **공개 API·wire·routing·part
  순서·DONTWAIT/consume 계약 불변.** 프로파일: `submitSend` −26%, `recv` −15%.
- **before→after (Core 0.17.5, tcp, runs=3, C 대비 %):**

| pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 평균 |
|---|--|--|--|--|--|--|--|
| MULTI_DEALER_ROUTER_SENDSEND | 24.2→26.3 | 20.9→24.6 | 18.7→24.5 | 20.9→**35.2** | 59.0→67.8 | 70.7→79.6 | 35.7→**43.0** |
| MULTI_ROUTER_ROUTER_SENDSEND | 27.8→28.4 | 37.9→41.2 | 32.9→38.1 | 38.0→43.4 | 48.4→47.0 | 42.3→43.6 | 37.9→40.3 |
| MULTI_DEALER_ROUTER_REQREP | 22.3→26.2 | 28.5→33.9 | 32.9→32.5 | 33.8→36.2 | 42.2→52.8 | 47.8→**63.1** | 34.6→**40.8** |
| MULTI_ROUTER_ROUTER_REQREP | 40.0→38.6 | 40.8→42.4 | 42.0→41.5 | 40.5→42.2 | 56.9→56.4 | 56.3→59.0 | 46.1→46.7 |

- 전 패턴 개선(최대 갭이던 DEALER_ROUTER SENDSEND/REQREP 평균 +7.3/+6.2pp). 몇 셀 −0.4~1.4는 3-run 변동 범위. **비대상 회귀 없음**
  (routed 65536 +2.9~6.4%, PAIR/PUBSUB 변동 내). 계약 테스트 27개+전체 npm test+샘플 7/7 통과.
- 남은 병목: 메시지당 N-API 경계 2회, receive envelope/parts/snapshot, async continuation. Node는 목표 median 60엔 아직 못 미치나 최대 갭
  패턴을 유의미하게 축소. **다음: dotnet DEALER_ROUTER_SENDSEND(갭 17.9)**.

## 12. 완료 기준

다음 조건을 모두 만족해야 작업을 완료한다.

- runner, 정책, 상세 표의 pattern, transport, size inventory가 일치한다.
- 각 pattern의 최종 판정에 사용한 core 0.17.5 C와 binding paired report가 모두
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
