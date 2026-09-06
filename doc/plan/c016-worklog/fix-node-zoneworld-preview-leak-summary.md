# D-112 Node ZoneWorld preview 프로세스 정리 수정

Node sample runner의 프로세스 소유권과 종료 처리를 수정했다. 이 기록은 감독자가 수정 범위,
누수 재현, 회귀 검사와 gate 결과를 검토하기 위한 자료다. commit은 수행하지 않았다.

## Baseline

2026-09-06 14:28:39 KST 기준으로 preview 3세트, 프로세스 9개를 확인했다.
작업 지시의 7개 서버와 달리 조사 시점에는 포트 28112·29623·28739만 남아 있었다.
각 shell의 PPID 4927은 `/init`이며 원래 runner와 PGID leader는 존재하지 않았다.
명령에 지정된 `/tmp/zlink-zoneworld-*` 디렉터리도 이미 삭제된 상태였다.
`/dev/shm/zlink-tmp-node/zlink-zoneworld-*`의 과거 디렉터리는 30개였으며 이 작업에서 정리하지 않았다.

`ps -o pid,ppid,pgid,lstart,args` 기록:

```text
  PID  PPID  PGID                  STARTED COMMAND
 4927  4926  4926 Fri Sep  4 21:19:41 2026 /init
34967  4927 23653 Sun Sep  6 12:51:02 2026 sh -c vite preview --host 127.0.0.1 --port 28112 --outDir /tmp/zlink-zoneworld-qZAmSa/work/zoneworld-browser-dist
34968 34967 23653 Sun Sep  6 12:51:02 2026 node /home/hep7/project/zlink/framework/languages/shared_sample/zoneworld/client/node_modules/.bin/vite preview --host 127.0.0.1 --port 28112 --outDir /tmp/zlink-zoneworld-qZAmSa/work/zoneworld-browser-dist
34976 34968 23653 Sun Sep  6 12:51:02 2026 /home/hep7/project/zlink/framework/languages/shared_sample/zoneworld/client/node_modules/@esbuild/linux-x64/bin/esbuild --service=0.28.1 --ping
38842  4927  1014 Sun Sep  6 13:47:55 2026 sh -c vite preview --host 127.0.0.1 --port 29623 --outDir /tmp/zlink-zoneworld-msQ6E9/work/zoneworld-browser-dist
38843 38842  1014 Sun Sep  6 13:47:55 2026 node /home/hep7/project/zlink/framework/languages/shared_sample/zoneworld/client/node_modules/.bin/vite preview --host 127.0.0.1 --port 29623 --outDir /tmp/zlink-zoneworld-msQ6E9/work/zoneworld-browser-dist
38861 38843  1014 Sun Sep  6 13:47:55 2026 /home/hep7/project/zlink/framework/languages/shared_sample/zoneworld/client/node_modules/@esbuild/linux-x64/bin/esbuild --service=0.28.1 --ping
88155  4927 81166 Sun Sep  6 14:07:53 2026 sh -c vite preview --host 127.0.0.1 --port 28739 --outDir /tmp/zlink-zoneworld-38Dplj/work/zoneworld-browser-dist
88156 88155 81166 Sun Sep  6 14:07:53 2026 node /home/hep7/project/zlink/framework/languages/shared_sample/zoneworld/client/node_modules/.bin/vite preview --host 127.0.0.1 --port 28739 --outDir /tmp/zlink-zoneworld-38Dplj/work/zoneworld-browser-dist
88164 88156 81166 Sun Sep  6 14:07:53 2026 /home/hep7/project/zlink/framework/languages/shared_sample/zoneworld/client/node_modules/@esbuild/linux-x64/bin/esbuild --service=0.28.1 --ping
```

종료 전에 `/proc/<pid>/stat`의 시작 시각과 명령을 저장했다. PID 재사용 여부를 다시 확인하고,
아래 PID에만 SIGTERM을 보낸 뒤 pidfd로 종료를 기다렸다. 다른 PGID 전체에 신호를 보내거나
프로세스 이름으로 일괄 종료하지 않았다.

```text
2026-09-06T14:29:41.038151+09:00 PID 34976: SIGTERM; exit observed
2026-09-06T14:29:41.043654+09:00 PID 34968: SIGTERM; exit observed
2026-09-06T14:29:41.043787+09:00 PID 34967: SIGTERM; exit observed
2026-09-06T14:29:41.044746+09:00 PID 38861: SIGTERM; exit observed
2026-09-06T14:29:41.051744+09:00 PID 38843: SIGTERM; exit observed
2026-09-06T14:29:41.051841+09:00 PID 38842: SIGTERM; exit observed
2026-09-06T14:29:41.052490+09:00 PID 88164: SIGTERM; exit observed
2026-09-06T14:29:41.057940+09:00 PID 88156: SIGTERM; exit observed
2026-09-06T14:29:41.058024+09:00 PID 88155: SIGTERM; exit observed
```

원본 기록: `/tmp/zlink-d112-preview-leak/baseline.json`, `baseline-processes.txt`,
`baseline-terminated.txt`, `baseline-workdirs.json`.

## 원인

아래 줄 번호는 수정 전 소스 기준이다. 복사본은 `/tmp/zlink-d112-preview-leak/*.before`에 있다.

- `framework/languages/node/samples/ZoneWorld/Runner/sample-runner.mjs:718,746`:
  `startCommand()`가 `npm exec vite preview`를 직접 실행하고 공통 `children`에 등록하지 않았다.
  자식은 `npm → sh → vite → esbuild`이며 별도 process group을 만들지도 않았다.
- 같은 파일 `:738-741`: 정상 경로의 `browser.dispose()`는 npm PID만 SIGKILL로 종료하고
  npm의 exit만 기다렸다. shell·Vite·esbuild는 종료 대상에 포함되지 않았다.
- 같은 파일 `:226-232`: browser 시작/readiness 실패는 지역 `try/finally` 밖에서 발생할 수 있었다.
  다른 scenario client, proxy, B8 자식 runner도 별도 spawn/종료 규칙을 사용했다.
- `framework/languages/node/samples/run-sample.mjs:476-493,518-521`:
  SIGTERM 처리에서 공통 목록만 정리한 뒤 `process.exit()`를 호출했다. ZoneWorld의 지역
  finally와 main의 디렉터리 삭제(`:77-82`)를 기다리지 않았다. 정상 cleanup도 SIGKILL 뒤의
  실제 종료를 기다리지 않았다.
- `framework/languages/node/samples/run_samples.sh:28`,
  `framework/languages/node/samples/ZoneWorld/run_sample.sh:10`:
  aggregate와 개별 shell이 자식에게 signal을 전달하거나 그 정리를 기다리지 않았다.
  `test/contract/sample-regression.test.js`의 `spawnSync(..., timeout: 600_000)`는 aggregate
  PID에 SIGTERM을 보내므로 이 경계에서도 전달이 필요하다.

소유 계층: Node sample runner. Framework runtime·Core·binding의 lifecycle은 변경하지 않았다.

계약 조항: `framework/doc/framework/common/sample/README.ko.md:444-445,454,482-485`의
실행별 PID/process handle·Redis ID 소유권과 정상/실패 정리 규칙.

교차언어 대조: .NET ZoneWorld는 `run_sample.sh:699`에서 preview PID를 `PIDS`에 등록하고,
`:90`의 EXIT trap에서 공통 helper를 호출한다. `samples/redis-common.sh:83`의 helper는
SIGTERM을 보내고 등록 PID를 wait한다. 이는 등록·종료 대기의 비교이며, .NET의 npm 하위
프로세스까지 정리되는지를 검증했다는 뜻은 아니다. 다른 언어 파일은 수정하지 않았다.

변경 분류: **B — 기존 sample runner 결함**.

## 수정

- 공통 `startCommand()`가 Node role, build, browser, scenario client, proxy와 B8 자식 runner를
  모두 등록한다. POSIX에서는 실행기가 만든 process group을 신호 대상으로 사용한다.
  stdout/stderr를 파일에 기록하면서, 하위 프로세스가 상속한 pipe까지 닫히는 `close`를 기다린다.
- 정상·예외·SIGINT·SIGTERM은 main의 같은 finally와 같은 cleanup promise로 수렴한다.
  종료 요청 뒤의 추가 spawn·port lease 취득을 차단한다.
- cleanup은 SIGTERM을 보내고 기존 500ms 한도 내에서 실제 종료를 기다린다. 한도를 넘긴 경우
  해당 소유 group만 SIGKILL로 종료하고, 종료를 기다린 후 cleanup 실패로 보고한다.
  timeout·retry 횟수·port 선택 규칙을 늘리거나 바꾸지 않았다.
- B8 시작 중 SIGINT가 Python import callback의 `KeyboardInterrupt`로 처리된 뒤 계속 실행되는
  사례를 기존 로그에서 확인했다. 종료 신호를 전체 소유 process에 대해 SIGTERM으로 통일했고,
  B8 중단 회귀 검사로 확인했다. proxy별 예외나 재시도는 추가하지 않았다.
- 기본 실행은 성공·실패·signal 종료 모두 run directory를 삭제한다. 실패 로그는 별도의
  `TMPDIR/zlink-sample-logs/<run-name>`에 권한 0700으로 보존한다. 명시적인 `--keep-run-dir`
  진단 옵션은 유지했다.
- 각 shell entry는 `exec node`를 사용한다. aggregate는 현재 sample PID에 signal을 전달하고
  자식 runner가 정리를 마칠 때까지 기다린다. ZoneWorld의 개별 dispose/kill 규칙은 삭제했다.

비교한 대안: ZoneWorld 지역 finally에서 npm 하위 PID를 따로 추적하는 방법과 공통 runner에
소유권을 모으는 방법을 비교했다. 공통 runner를 선택해 정상·실패·signal 경로의 규칙 중복을 줄였다.

수정 전/후 규칙 수: **5 → 1** — 공통 PID 정리, browser dispose, scenario client dispose,
proxy 정리, signal 전용 종료를 공통 소유 group 정리 규칙으로 통합했다.

변경 파일:

- `framework/languages/node/samples/run-sample.mjs`
- `framework/languages/node/samples/run_samples.sh`
- `framework/languages/node/samples/ZoneWorld/Runner/sample-runner.mjs`
- `framework/languages/node/samples/ZoneWorld/test/runner-cleanup.test.js`
- `framework/languages/node/samples/{Bingo.Ts,DeliveryDispatch.Ts,SupportChat.Ts}/Runner/sample-runner.mjs`
  — 공통 browser 실행이 비동기로 바뀐 호출부의 await.
- 7개 sample의 `run_sample.sh` — Node로 exec.
- 이 요약 문서.

`core/**`, `bindings/**`, spec·Framework 문서, 다른 언어, Node `packages/**`·`test/**`는 수정하지 않았다.

## 회귀 검사

`ZoneWorld/package.json`의 기존 `test` 명령이 찾는 `test/runner-cleanup.test.js`에 추가했다.
실제 aggregate runner로 정상 실행, preview가 준비된 뒤 aggregate PID에 SIGTERM, B8 proxy 시작
중 SIGTERM을 검사한다. 실행 중 `/proc`에서 parent chain과 PID 시작 시각을 기록한다.
runner exit 직후 소유 프로세스와 두 run directory가 모두 없어야 통과한다. 정리 후 잠시 기다려
누수를 감추는 방식은 사용하지 않는다. 실패한 검사도 관찰한 자기 PID만 종료하고 기록한다.

수정 전 소스 복사본으로 정상 실행 검사를 수행하여 **예상대로 실패**했다. 포트 29219의
shell 25510, Vite 25511, esbuild 25519가 남았고 검사가 세 PID를 검출·종료했다.
기록: `/tmp/zlink-d112-preview-leak/regression-before-fix.log` 및
`/tmp/zoneworld-cleanup-normal-H1aFJT/processes.json`.

중간 B8 중단 진단에서 남은 proxy PID 51350은 회귀 검사가 기록 후 종료했고, 그 실행이 만든
Redis ID `6fc2e6620b6f594baab55c33fc35526654e8d39cb51658c328bf6423b45265f4`만 제거했다.
실패 로그를 보존한 뒤 두 진단 실행의 작업 디렉터리도 제거했다. 기록은
`/tmp/zlink-d112-preview-leak/b8-regression-redis-cleanup.txt`, `diagnostic-workdir-cleanup.txt`에 있다.

최종 회귀 검사 **3/3 통과**. `/tmp/zlink-d112-preview-leak/regression-final.log`.

```bash
TMPDIR=/dev/shm/zlink-tmp-node flock /tmp/zlink-samples-gate.lock \
  flock /tmp/zlink-node-gate.lock node --test \
  framework/languages/node/samples/ZoneWorld/test/runner-cleanup.test.js
```

| 종료 경로 | 관찰한 소유 PID | preview 계열 PID | esbuild PID | 삭제한 run directory | 잔류 PID |
|---|---:|---:|---:|---:|---:|
| 정상 | 78 | 2 | 2 | 2 | 0 |
| preview 준비 후 SIGTERM | 61 | 2 | 3 | 2 | 0 |
| B8 proxy 시작 중 SIGTERM | 33 | 0 | 1 | 2 | 0 |

프로세스 표에는 npm/shell wrapper와 일회성 build service도 포함한다.
각 검사의 `run.log`와 `processes.json`은 결과 로그의 `evidence=` 경로에 보존했다.

관련 정적 검사: 공통 runner/aggregate contract 4/4, ZoneWorld gate contract 10/10 통과.
`bash -n`, Node syntax 검사, 범위 내 `git diff --check` 통과.

## Gate

최종 gate **7/7 통과, exit 0**.
TicTacToe.Ts, Bingo.Ts, DeliveryDispatch.Ts, SupportChat.Ts, GameQuest.Ts, ShoppingMall.Ts,
ZoneWorld가 모두 `sample <name> completed`를 출력했다.

```bash
TMPDIR=/dev/shm/zlink-tmp-node flock /tmp/zlink-samples-gate.lock \
  flock /tmp/zlink-node-gate.lock bash framework/languages/node/samples/run_samples.sh
```

로그: `/tmp/zlink-d112-preview-leak/samples-gate.log`, `samples-gate-exit.txt`.

`pgrep -af '[v]ite preview|[e]sbuild --service'` 결과는 **gate 전 0개 → 후 0개**였다.
`/dev/shm/zlink-tmp-node/zlink-zoneworld-*`는 **전 30개 → 후 30개, 새로 남은 디렉터리 0개**다.
각각 `gate-before-processes.txt`, `gate-after-processes.txt`, `gate-before-workdirs.json`,
`gate-after-workdirs.json`에 기록했다. Linux/WSL에서 검증했으며 Windows gate는 실행하지 않았다.

## BLOCKERS

`framework/languages/node/test/contract/sample-runner-teardown.test.js:16-21`의 VM fixture가
`setTimeout`·`clearTimeout`을 제공하지 않아 원본 테스트 3개가 실패한다. 같은 fixture의 role은
SIGINT만 처리하므로 공통 종료 신호인 SIGTERM도 처리하도록 fixture를 갱신해야 한다.
fixture에 `closed`와 `exited`도 실제 runner처럼 close 이벤트로 제공해야 한다.
assertion 변경은 필요하지 않다. 해당 파일은 다른 작업 소유 범위여서 수정하지 않았다.

임시 복사본에 timer global·close 완료 상태·SIGTERM 처리를 제공하고 assertion을 그대로 실행하면 **4/4 통과**한다.
수정 제안과 결과:
`/tmp/zlink-d112-preview-leak/teardown-contract-updated-fixture.test.js`,
`/tmp/zlink-d112-preview-leak/teardown-contract-updated-fixture.log`.
원본 실패 기록: `/tmp/zlink-d112-preview-leak/teardown-contract.log`.
