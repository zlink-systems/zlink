**#4163 — caller mode와 code layout 비교**

PM의 고정 규칙에 따른 판정은 **semantic change 조건 충족**이다. R의 high가 기준값과 같은 3/10이다. 해당 결정은 complete send scope와 지정 caller가 `pipe_write_complete_record` 대신 `pipe_write_incremental`을 전달하는 것이다. 다만 **동일 세션 C도 3/10**이므로 R이 C의 high 비율을 낮췄다는 인과효과는 확인하지 못했다. 이 판정 조건만으로 의미 변경이 원인이라고 단정할 수 없다. L 5/10과 C 3/10의 차이는 layout 효과를 확정할 근거로 부족하다.

모든 실험 source의 기준은 `0005a4d6c0`이다. R은 `9dc2246dd0`의 기존 `true, true` HWM 동작에 대응하도록 지정한 caller 5곳만 바꿨다. L은 source 변경 없이 Core의 `CMAKE_CXX_FLAGS`에 `-falign-functions=64`만 추가했다. compiler 기본값은 16이다. Release `-O3`, LTO, `-g` 없음과 debug section 부재를 확인했다.

| 빌드 | 변경 | libzlink.so SHA256 앞 16자리 | Raw |
|---|---|---|---|
| C | 기존 shared control | `2e0aa58e221f0cbe` | [provenance](logs/C/core-package-provenance.json) |
| R | caller mode만 incremental | `54fb60a79f9ea883` | [diff](R.diff), [compile](logs/R/core-compile_commands.json) |
| L | 함수 정렬 64바이트 | `13853f595a18f603` | [source diff: 0 bytes](L.diff), [compile](logs/L/core-compile_commands.json) |

전체 SHA와 symbol 주소는 [layout-check.json](logs/layout-check.json)에 있다. `write_message_unlocked` 주소는 C `0xa8230` → L `0xabac0`으로 바뀌었다. C prefix는 `/root/jobs/fx/cx/v21/s033/core124/prefix-1.24`다. R·L과 probe R·L prefix는 이 JOB의 `prefix-{R,L,probe-R,probe-L}`에 보존했다.

비계측은 C→R→L, L→R→C 순서로 각 `--runs 5`, single ROUTER_ROUTER/inproc, 256·1024B를 실행했다. 이전 recipe의 환경과 `flock -o measure.lock`, CPU 8–15를 유지했다. high는 **1024B mean ≥ 0.9ms**이며, 계측 실행은 이 횟수에 합치지 않았다. 표의 mean은 실행별 mean의 중앙값(최소–최대), 단위는 ms다.

| 빌드 | 1024B high: r1+r2=전체 | 1024B mean | 256B mean | Raw |
|---|---:|---:|---:|---|
| C | 3+0=3/10 | 0.1195 (0.003–1.152) | 2.6020 (1.899–3.475) | [r1](runs/C-r1-single-ROUTER_ROUTER-inproc.log), [r2](runs/C-r2-single-ROUTER_ROUTER-inproc.log) |
| R | 2+1=3/10 | 0.1385 (0.097–1.108) | 2.3390 (1.128–3.525) | [r1](runs/R-r1-single-ROUTER_ROUTER-inproc.log), [r2](runs/R-r2-single-ROUTER_ROUTER-inproc.log) |
| L | 2+3=5/10 | 0.5735 (0.042–1.142) | 3.1930 (0.161–3.373) | [r1](runs/L-r1-single-ROUTER_ROUTER-inproc.log), [r2](runs/L-r2-single-ROUTER_ROUTER-inproc.log) |

실행별 값은 [runs-10.csv](runs-10.csv), 시각·SHA·원시 출력 경로는 [times.tsv](runs/times.tsv), runner 저장 결과는 [saved-results](runs/saved-results/)에 있다.

Depth는 이전 `instrument.py`와 동일한 counter로 1,024 commits마다 표본을 수집했다. R·L의 counter diff가 이전 C probe와 같은지도 `cmp`로 확인했다. C는 기존 `/root/jobs/fx/cx/v21/s033/f4163/prefix-probe-C`를 읽기 전용으로 재사용하고, C→R→L 각각 5회 실행했다. 각 raw에 `ldd`가 있다. 마지막 누적 dump의 commits는 7,340,032 또는 8,388,608이며, 최신 commit의 flush 전 관측으로 depth에는 최대 1 record 차이가 있다.

각 cell은 **mean ms / sampled 평균 depth records**이며 링크는 해당 실행 raw다.

| 실행 | C | R | L |
|---|---:|---:|---:|
| 1 | [0.023629 / 43.36](runs/probe-C-1.log) | [0.035559 / 59.62](runs/probe-R-1.log) | [0.002862 / 3.97](runs/probe-L-1.log) |
| 2 | [1.086824 / 2,054.71](runs/probe-C-2.log) | [0.950227 / 1,844.43](runs/probe-R-2.log) | [0.095876 / 172.48](runs/probe-L-2.log) |
| 3 | [1.184432 / 2,068.56](runs/probe-C-3.log) | [1.057073 / 2,019.43](runs/probe-R-3.log) | [0.075162 / 130.21](runs/probe-L-3.log) |
| 4 | [0.110224 / 201.55](runs/probe-C-4.log) | [0.961720 / 1,871.13](runs/probe-R-4.log) | [0.003254 / 4.50](runs/probe-L-4.log) |
| 5 | [0.057051 / 105.77](runs/probe-C-5.log) | [0.628822 / 1,250.73](runs/probe-R-5.log) | [0.308477 / 576.51](runs/probe-L-5.log) |
| **depth 중앙값** | **201.55** | **1,844.43** | **130.21** |

전체 counter는 [probe-summary.csv](probe-summary.csv), 계측 diff는 [R](probe-R.diff)·[L](probe-L.diff), recipe는 [build.sh](build.sh)·[build-probe.sh](build-probe.sh)·[measure.sh](measure.sh)·[probe-run.sh](probe-run.sh)다. 계측 결과는 depth 관측이며 비계측 판정과 분리했다.

검증: 비계측 60개 결과와 probe 15개 결과, 6개 stage의 정상 종료, SHA·설정·source 범위 검사를 통과했다. 남은 실행 실패는 없다. 소유 계층은 공통 Core pipe admission이며 기준은 `core/doc/spec/core/systems/06-auto-hwm.ko.md:453,470,476`이다. Framework·binding 변경은 없고 언어별 경로를 추가하지 않았다. 이번 변경은 PM 승인 실험이며 수정안 A/B/C/D 채택은 하지 않았다. mode 지정 위치는 5→5이고 새 상태·flag·queue·lock은 0개다. 변경 범위·중복 판정 점검의 추가 발견은 0건이다.

자원 사전 검사 44회 모두 통과했다. 최소 MemAvailable 18.32GiB, 기록된 최대 JOB 1.16GiB / 25GiB다. [검사 결과](logs/final-integrity.json). 기존 f4163의 recipe·C probe prefix·브랜치는 보존했다. push·PR·GitHub·spec 변경과 추가 수정은 없다. 이 작업의 build 디렉터리와 detached worktree는 정리했으며 raw와 prefix는 보존했다.
