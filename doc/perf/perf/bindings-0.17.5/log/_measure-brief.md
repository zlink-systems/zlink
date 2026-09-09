# bindings 0.17.5 성능 측정 job (codex 서브에이전트 브리프)

너는 zlink 저장소에서 **성능 측정만** 수행하는 실행 에이전트다. 감독자(머신 B)가
결과를 받아 계획 문서를 직접 갱신한다. 아래 규칙과 이번 job의 스코프를 정확히 따른다.

## 0. 절대 규칙 (위반 시 즉시 중단하고 보고)

1. **측정은 절대 병렬 실행하지 않는다.** 한 번에 perf 러너 프로세스 하나만 실행하고,
   그 프로세스가 끝나고 report `status: complete`를 확인한 뒤 다음 하나를 시작한다.
   백그라운드로 두 번째 perf 프로세스를 띄우지 않는다.
2. **Core source를 다시 build하지 않는다.** 측정 runtime은 GitHub release `core/v0.17.5`이며
   러너에 `--core-version 0.17.5`를 전달해 선택한다. 러너가 report META의 `core_version`을
   `0.17.5`로, `core_source`를 release로 보고하는지 확인한다. 0.17.5가 아니면 즉시 중단하고
   실제 값과 함께 보고한다. (0.17.5는 아직 캐시에 없을 수 있어 첫 실행에서 release 자산을
   내려받는다. 다운로드 후 실제 runtime 경로와 provenance를 최종 보고에 적는다.)
3. **문서(계획서·스펙·정책)를 수정하지 않는다.** 너의 출력은 `log/` 아래의 결과 TSV와
   최종 보고뿐이다. `doc/perf/perf/bindings-0.17.5/*.md` 계획서를 편집하지 않는다.
4. **branch는 `main`이다.** commit·push·branch 전환·reset·restore를 하지 않는다. 측정으로
   생성되는 report 산출물 외에 소스/문서를 변경하지 않는다.
5. 러너가 지원하지 않는 transport/pattern/option은 건너뛰고 그 사실을 보고한다. 지원하지 않는
   option을 성공으로 받아들이고 무시하는 러너가 있으면 그 사실을 보고한다(수치를 만들지 않는다).
6. timeout 증가·sleep·retry·client 수 축소로 실패를 숨기지 않는다. report가 partial이거나
   `status: complete`가 아니면 그 셀을 실패로 기록하고 원인(첫 오류 줄)을 남긴다.

## 1. 필독 (측정 의미 정렬)

- `doc/perf/perf/bindings-0.17.5/bindings-library-performance-improvement-plan-core-0.17.5.ko.md`
  의 §3(측정 크기), §7.0~§7.3(측정 단위·순차 실행·parity gate·paired C 규칙), §8(판정·기록).
- `doc/perf/PERF_SINGLE_TEST_POLICY.md`, `doc/perf/PERF_MULTI_TEST_POLICY.md` (측정 의미).
- 공식 entrypoint만 사용한다:
  - C single: `bindings/c/perf/run_benchmarks.sh`
  - C multi: `bindings/c/perf/run_benchmarks_multi.sh`
  - binding single: `bindings/<lang>/perf/run_benchmarks.sh`
  - binding multi: `bindings/<lang>/perf/run_benchmarks_multi.sh`

## 2. 측정 크기 (반드시 명시적으로 전달)

- Single 모든 pattern: `--msg-sizes 64,256,1024,65536,131072,262144`
- Multi 대부분 pattern: `--msg-sizes 64,256,1024,4096,65536,131072`
- Multi `MULTI_STREAM`만: `--msg-sizes 64,256,1024,65536` (4096·131072는 `해당 없음`)

## 3. 실행 절차 (이번 job의 transport 하나에 대해)

이번 job은 하나의 (언어, suite, transport)만 측정한다. 아래를 순서대로 한다.

1. `git branch --show-current`가 `main`인지 확인한다.
2. **smoke**: 먼저 C 러너를, 다음 binding 러너를 `--duration 1 --runs 1 --msg-sizes 64`로,
   이번 transport의 대표 pattern 1개에 대해 각각 한 번씩 실행해 `status: complete`인지
   확인한다. 실패하면 원인을 보고하고 중단한다.
3. **본 측정**: 이번 transport에 대해, 해당 suite의 각 pattern을 (또는 러너가 허용하면
   `--pattern <comma-list>`로 한 번에) 다음 조건으로 측정한다.
   - `--core-version 0.17.5`
   - `--transports <이번 transport>`
   - suite에 맞는 `--msg-sizes` (§2)
   - `--duration 5 --runs 1`
   - `--results-tag c0175-<lang>-<suite>-<transport>` (C·binding 동일 tag = pair tag)
   먼저 C 러너 전체 → 끝나고 status 확인 → 같은 조건으로 binding 러너 전체. 절대 동시 실행 금지.
   MULTI_STREAM은 크기가 다르므로 같은 transport 안에서 별도 러너 호출로 측정한다.
4. 각 report에서 `RESULT,current,<pattern>,<transport>,<size>,<metric>,<value>` 라인을
   읽어 throughput(ops/s)과 latency(ms, metric=`latency`=평균)를 뽑는다. `## Completion`의
   `status`도 읽는다.

## 4. 결과 기록 (너의 유일한 산출 파일)

`doc/perf/perf/bindings-0.17.5/log/results-<lang>-<suite>.tsv`에 **append**한다(없으면 헤더와
함께 생성). 한 줄 = 하나의 (pattern, transport, size). 탭 구분, 컬럼:

```
lang	suite	transport	pattern	size	c_tput_ops	bind_tput_ops	tput_ratio_pct	c_lat_ms	bind_lat_ms	lat_ratio	c_status	bind_status	c_report	bind_report	pair_tag
```

- `tput_ratio_pct` = bind_tput/c_tput*100 (소수 1자리). `lat_ratio` = bind_lat/c_lat (소수 2자리).
- c_report/bind_report는 저장된 report 파일의 저장소 상대 경로.
- 값이 없으면 (실패/미측정) 해당 칸에 `NA`, 상태 칸에 실패 원인 요약.

## 5. 최종 보고 (감독자에게 텍스트로)

- 이번 (언어, suite, transport)의 결과 TSV 경로.
- C·binding report META의 실제 `core_version`, `core_source`, `core_runtime`.
- Effective Options 일치 여부(io_threads, duration, msg_sizes, transports), auto-HWM `MsgUnit(B)`
  일치 여부, 실제 client 수(multi), memory guard cap 발생 여부.
- pattern×size별 tput_ratio(%)와 lat_ratio를 표로 요약. `status`가 complete 아닌 셀 목록.
- 스코프 밖은 건드리지 않았음을 확인.
