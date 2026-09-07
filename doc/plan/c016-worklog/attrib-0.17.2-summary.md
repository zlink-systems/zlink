# attrib-0172: 0.17.2 perf/c 하락 귀속

## 판정

**기준 불일치(§7.4 Phase 2G 값의 측정 조건 차이)**로 판정한다. 지정 판정 규칙인
“두 교대 모두 base 대비 5% 이상 하락”을 만족한 셀이 없다. 따라서 이 교차 측정만으로는
MP 계열 회귀 후보를 특정하지 않는다. 원인 코드 분석은 수행하지 않았다.

1024 B에서 첫 교대만 크게 하락한 값은 있었지만 두 번째 교대에서 재현되지 않았다.
특히 multi DR_REQREP은 79.70% 뒤 98.35%였고, 이전 Phase 2G 절대 기준(208.5 Kops/s)과의
18.1% 차이는 base/current 라이브러리 차이로 귀속할 수 없다.

## 범위와 runtime 고정

- base: detached worktree `/home/hep7hep7/project/zlink-work/base-5304885197`, commit `5304885197`,
  `JOBS=4 scripts/build-core.sh release --lib-only`로 만든
  `core/build/lib/libzlink.so.0.17.1` (04:54:37 KST).
- current: main `core/build/lib/libzlink.so.0.17.2` (03:57:35 KST 갱신본).
- single은 같은 `bindings/c/build/perf/*` binary와 `--reuse-build`를 사용했다. 이 runner는
  local main runtime을 다시 prepend하므로, versioned `LD_PRELOAD`로 둘 중 하나만 주입했다.
- multi shell runner는 binary RUNPATH와 실제 runtime이 다르면 의도적으로 거부한다. 같은
  `bindings/c/build/perf/*` binary, 동일 options/duration/clients를 쓰는 기존
  `bindings/c/perf/run_comparison.py` backend를 직접 실행해 그 사전 검증만 건너뛰었다.
  build 또는 source 변경은 없었다.
- 각 교대에서 `LD_DEBUG=libs` loader trace를 남겼다. base 교대는
  `.../base-5304885197/core/build/lib/libzlink.so.0.17.1`, current 교대는
  `.../zlink/core/build/lib/libzlink.so.0.17.2`만 `calling init`에 나타났고 반대 version은 0건이다.
  single은 교대별 46개, multi는 14~44개의 loader trace가 이를 확인한다.

## 유휴와 실행 상태

외부 `zlink-work/st1` 컴파일 때문에 시작 전 두 시도는 폐기했다(하나는 single runtime override가
main을 다시 prepend함, 하나는 시작 load 3.79). 외부 build가 05:03:30 KST에 끝난 뒤
single 전 05:05:08~05:07:18, multi 전 05:13:18~05:15:27에 1분 load가 1.0 미만이고 ninja와
compiler가 없는 상태를 2분 이상 확인했다. 모든 valid 실행은 지정 `PERF_LOCK` 아래 포그라운드로
직렬화했다.

| suite | 교대 순서 | 시작 load 1/5/15 | ninja | 결과 |
|---|---|---:|---:|---|
| single | base1 → current1 → base2 → current2 | 0.47/1.51/2.40 → 1.39/1.58/2.36 → 1.46/1.56/2.29 → 1.67/1.60/2.25 | 모두 0 | 각 4/4 성공 |
| multi | base1 → current1 → base2 → current2 | 0.15/0.75/1.69 → 3.60/1.57/1.92 → 4.03/1.95/2.03 → 3.96/2.21/2.12 | 모두 0 | 각 2/2 성공 |

후속 교대의 높은 1분 load는 직전 5초 active window와 100 client 프로세스가 남긴 load average다.
모든 비교쌍은 같은 lock 안에서 바로 교대했다.

## 교차 결과

처리량은 single Kmsg/s, multi Kops/s이다. `current/base`가 95% 이하인 두 교대가 모두 있어야
회귀 후보인데, 어느 행도 그렇지 않다.

| 셀 | base1 | current1 | 비율 1 | base2 | current2 | 비율 2 | base 중앙값 | current 중앙값 | 합산 비율 | 교대 판정 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| single PAIR tcp 1024 B | 799.4 | 749.0 | 93.70% | 761.2 | 733.7 | 96.38% | 780.3 | 741.3 | 95.00% | 1회만 ≤95% |
| single DEALER_DEALER tcp 1024 B | 815.1 | 766.4 | 94.03% | 773.8 | 752.9 | 97.30% | 794.4 | 759.7 | 95.62% | 1회만 ≤95% |
| single PAIR tcp 65536 B | 41.49 | 41.52 | 100.07% | 40.83 | 40.58 | 99.39% | 41.16 | 41.05 | 99.73% | 일관 하락 없음 |
| single DEALER_DEALER tcp 65536 B | 42.11 | 41.87 | 99.44% | 40.14 | 41.12 | 102.45% | 41.13 | 41.50 | 100.91% | 일관 하락 없음 |
| multi DEALER_ROUTER_REQREP tcp 1024 B | 249.7 | 199.0 | 79.70% | 203.3 | 199.9 | 98.35% | 226.5 | 199.5 | 88.07% | 1회만 ≤95% |
| multi ROUTER_ROUTER_SENDSEND tcp 1024 B | 257.5 | 243.8 | 94.69% | 237.0 | 230.4 | 97.21% | 247.2 | 237.1 | 95.90% | 1회만 ≤95% |

raw 결과는 다음 파일에 남겼다.

- `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_050739_attrib-0172-single-base1-valid.txt`
- `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_050852_attrib-0172-single-current1-valid.txt`
- `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_051005_attrib-0172-single-base2-valid.txt`
- `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_051117_attrib-0172-single-current2-valid.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_051644_attrib-0172-multi-base1-valid.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_051731_attrib-0172-multi-current1-valid.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_051818_attrib-0172-multi-base2-valid.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_051905_attrib-0172-multi-current2-valid.txt`

## 변경과 검증

- 변경 파일: 이 요약과 `progress-attrib-0172.md`뿐이다. `core`, `bindings`, `scripts`의 git status는 비어 있다.
- 실행: base Release lib-only build 1회, valid single 4회(각 4/4), valid multi 4회(각 2/2).
- 소스·스펙·커밋·stash 변경은 없었다. base worktree는 요청대로 남겼다.
