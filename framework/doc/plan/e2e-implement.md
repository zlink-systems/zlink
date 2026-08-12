  Goal은 다음 중 하나일 때만 종료해.

  1. 모든 scenario와 최종 검토가 `CLEAN`으로 검증됨
  2. 아래 중단 조건에 해당하는 실제 blocker가 충분한 근거와 함께 확인됨

  현재 `main`을 기준으로 ZLink Framework common E2E의 모든 scenario를
  C++, .NET, Java, Kotlin, Node에서 구현하고 실제 process로 검증해.

  조사, 구현, 검증과 최종 판단에 필요한 세부 agent 구성, 병렬화 정도와 작업 순서는
  상황에 맞게 결정해. 단, 아래에서 요구하는 coordinator, 독립 integration reviewer와
  shared integration owner의 책임은 분리해서 유지해.

  ## 1. 시작 기준과 작업공간

  작업을 시작할 때 먼저 active ZLink repository와 git root를 확인해.

  `git fetch origin` 후 당시 `origin/main`의 SHA를 `BASE_SHA`로 기록하고,
  common E2E scenario inventory를 `BASE_SHA`에서 직접 산출해. 과거 ledger, 기존 runner
  목록, 다른 언어 구현이나 이전 실행 log를 현재 inventory의 근거로 사용하지 마.

  기존 checkout에 unrelated dirty change가 있으면 이를 수정, 삭제, stash 또는 overwrite하지
  마. 병렬 agent가 source를 수정할 때는 isolated worktree와 작업 branch를 사용하거나,
  서로 겹치지 않는 경로 소유권을 명확히 분리해.

  작업 중 `origin/main`이 이동해도 서로 다른 기준의 source와 evidence를 자동으로 섞지 마.
  upstream 변경을 조사하고 contract, common E2E, runner 또는 production 동작에 영향이 있으면
  영향받는 완료 scenario를 다시 열고 새 기준에서 재검증해.

  ## 2. 핵심 진행 방식: scenario vertical slice

  핵심 진행 단위는 language가 아니라 scenario vertical slice다.

  - 현재 common E2E 문서에서 scenario inventory와 정확한 scenario ID를 직접 확인한다.
  - 의존성과 공통 기반을 고려해 다음 scenario 하나를 선택한다.
  - 해당 scenario의 계약, 문서, 다섯 언어 구현과 실제 실행을 함께 검토한다.
  - 언어별 source 작업은 가능한 범위에서 병렬로 진행한다.
  - 다섯 언어와 필요한 cross-language 검증이 모두 승인되기 전에는 다음 scenario로
    넘어가지 않는다.
  - 공통 해석이나 production 변경이 이전 scenario에 영향을 주면 해당 scenario를
    `REOPENED`로 되돌리고 재검증한다.
  - 이 과정을 inventory의 모든 scenario가 닫힐 때까지 반복한다.

  scenario를 임의로 `N/A`, skip 또는 covered-by-other-test로 처리하지 마. 한 언어의 exact
  interface가 해당 common 동작을 제공해야 하는데 구현이 없다면 implementation gap으로
  처리해. exact interface 자체가 common 동작과 충돌하거나 동작을 정의하지 않는다면
  contract blocker로 처리해.

  ## 3. 계약 기준과 수정 금지 범위

  각 scenario를 시작할 때 common E2E 문서를 다음 계약 기준에 대조해.

  1. `framework/doc/framework/common/spec/`
  2. `framework/doc/framework/common/internals/`
  3. 언어별 exact interface

  common spec과 언어별 exact interface가 public contract의 기준이다.

  다른 언어 구현, 기존 E2E 코드, runner, fixture, 과거 log 또는 common E2E 문서 자체를
  새 public contract의 근거로 사용하지 마.

  다음 문서는 절대 수정하지 마.

  - `framework/doc/framework/common/spec/`
  - `framework/doc/framework/common/internals/`
  - 위 경로에 포함된 언어별 exact interface와 spec·internals 문서

  `framework/doc/framework/common/e2e/`는 다음 조건을 모두 만족할 때만 수정할 수 있어.

  - 기존 spec의 의미가 명백하다.
  - 새 public contract가 필요하지 않다.
  - scenario 문서의 오류, 누락 또는 잘못된 검증 방식만 바로잡는다.
  - 한국어와 영어 문서를 함께 일치시킨다.

  사용자 관찰 동작이 spec 사이에서 충돌하거나, exact interface와 common spec의 해석이
  갈리거나, spec에 없는 동작 또는 새 public API가 필요하면 문서와 구현을 수정하지 마.
  중단 조건에 따라 정확한 `file:line`, 다섯 언어 영향, 가능한 선택지와 각 선택지의
  trade-off를 보고해.

  ## 4. scenario 사전 독립 검토 게이트

  각 scenario의 언어별 구현을 시작하기 전에 별도의 독립 integration reviewer sub-agent가
  scenario 자체를 read-only로 검증해.

  reviewer는 해당 scenario의 언어별 implementer나 문서 수정자를 겸하지 않으며,
  가능하면 fresh context에서 계약 문서를 직접 조사해. implementer는 자신이 구현한
  scenario를 승인할 수 없어.

  reviewer는 최소한 다음을 검증해.

  - common spec의 정확한 계약 근거와 `file:line`
  - 다섯 언어 exact interface의 대응 surface와 `file:line`
  - 실제 배포 topology와 필요한 process 역할
  - scenario의 시작 상태와 readiness 조건
  - 사용자가 관찰해야 하는 정상 결과
  - 기대 실패, timeout, 금지 결과와 visible failure
  - client evidence와 실제 role-server evidence
  - cross-language 검증 필요 여부와 필요한 조합
  - child process, Redis, browser, port, run directory와 임시 자원의 cleanup 방식
  - 검증 방식이 private API, raw frame 또는 test-only 우회를 사용하지 않는지
  - scenario 문서가 public contract보다 강하거나 약한 보장을 요구하지 않는지

  reviewer는 다음 중 하나를 판정해.

  - `VALIDATED`
    - 계약과 검증 방식이 명확하며 구현을 시작할 수 있음
  - `DOC_FIX_REQUIRED`
    - 계약은 명확하지만 common E2E 문서만 잘못되었거나 불완전함
  - `CONTRACT_BLOCKED`
    - spec 부재, 계약 충돌, 해석 불일치, 새 public API 또는 현재 범위 밖 변경이 필요함

  `DOC_FIX_REQUIRED`이면 shared integration owner가 common E2E 한국어·영어 문서를 함께
  수정하고 필요한 documentation 검증을 실행해. 그 후 reviewer가 새로운 review turn에서
  수정된 문서를 다시 검증해야 해.

  `VALIDATED` 판정을 받은 scenario만 다섯 언어 구현을 시작해.
  `CONTRACT_BLOCKED`이면 구현하지 말고 중단·보고해.

  ## 5. 언어별 구현과 shared ownership

  다섯 언어는 동일한 public 동작과 사용자 관찰 결과를 제공해야 해. 언어 primitive,
  내부 concurrency 구조와 private 구현까지 억지로 같게 만들 필요는 없어.

  한 언어에서 문제를 발견하면 그 언어만 국소적으로 통과시키지 마. 같은 원인이 다른
  runtime에도 있는지 common contract와 각 구현을 대조해 종합적으로 판단해.

  공통 원인이면 영향받는 모든 언어에 함께 적용하고 동일한 scenario를 새 candidate SHA에서
  다시 검증해.

  shared protocol, generated asset, cross-language fixture, 공통 runner와 공통 verification
  script는 한 명의 shared integration owner만 수정하고 통합해. 각 language agent가 shared
  file을 서로 다르게 수정하거나 직접 통합하지 않게 해.

  coordinator가 다음을 최종 소유해.

  - scenario inventory와 durable ledger
  - shared change 통합
  - cross-language 결과 판단
  - 최종 staging, commit과 push
  - scenario 승인 또는 거절
  - 최종 `CLEAN` 판정

  ## 6. 실제 E2E 구현 원칙

  E2E는 실제 사용 흐름이어야 해.

  - 실제 역할 server를 별도 process로 실행한다.
  - scenario가 정의한 실제 public entrypoint를 사용한다.
  - HTTP 또는 stream 기반 scenario에서는 공개 HTTP client 또는 stream connector를
    사용한다.
  - 그 밖의 scenario도 해당 언어 exact interface의 public API만 사용한다.
  - client-visible 결과와 실제 역할 server evidence를 함께 확인한다.
  - readiness는 fixed sleep이 아니라 public 상태, 역할별 ready evidence 또는 명시적
    barrier로 확인한다.
  - test-only driver, private API, internal import, reflection, raw-frame 우회와 성공이
    고정된 marker를 사용하지 않는다.
  - 정상, 실패와 timeout 경로 모두에서 child process, Redis, browser, browser profile,
    port, run directory와 임시 자원을 정리한다.
  - 각 process의 exit code와 원본 역할별 log를 보존한다.
  - source-level test, package/clean-consumer 증거와 실제 process 결과를 서로 다른 검증
    lane으로 기록한다.

  harness 재실행, timeout 증가 또는 비계약적 retry로 실패를 숨기지 마. 단, spec이
  보장하는 production retry 동작 자체는 scenario의 검증 대상에 포함해.

  실패가 발생하면 client의 요약 오류만으로 원인을 단정하지 말고 client log, 역할 server
  log, correlation evidence, process exit status와 cleanup 결과를 함께 조사해.

  ## 7. 실행 격리와 병렬화

  source 구현은 병렬로 진행할 수 있어.

  실제 process test는 다음 항목이 완전히 격리되는지 먼저 판단해.

  - port
  - Redis instance, namespace와 database
  - browser와 browser profile
  - run directory
  - build output
  - package/cache directory
  - environment variable
  - background child process

  격리를 증명할 수 있으면 병렬 실행하고, 확신할 수 없으면 순차 실행해. 작은 속도 향상을
  위해 flaky 또는 cross-test contamination 위험을 받아들이지 마.

  process test가 실패한 뒤 단순 재실행으로 통과하면 성공으로 승인하지 마. 실패 원인을
  확인하고 isolation 또는 production 문제를 해결한 뒤 반복 실행에서 안정성을 검증해.

  ## 8. production 변경과 regression

  Framework production을 수정했으면 실제 owner layer에 regression test를 추가해.

  변경 후 다음을 영향 범위에 맞게 다시 실행해.

  - owner-layer unit/regression
  - language source test
  - API/exact-interface verification
  - 새 package 생성
  - isolated clean-consumer/package verification
  - 해당 scenario의 실제 process test
  - 필요한 cross-language test

  같은 version의 기존 global package cache를 새 package의 증거로 재사용하지 마.
  candidate SHA에서 package를 새로 생성하고 isolated cache와 consumer를 사용해 package
  provenance를 확인해.

  Core, bindings, protected spec/internals 또는 public contract 변경처럼 현재 Framework E2E
  범위를 넘어가는 수정이 필요하면 우회 구현하지 말고 중단·보고해.

  ## 9. scenario SHA와 완료 조건

  각 scenario의 통합 candidate를 `SCENARIO_SHA`로 기록해.

  다섯 언어의 source, package와 process evidence는 동일한 `SCENARIO_SHA`에서 생성되어야 해.
  검증 중 source가 변경되면 이전 결과를 그대로 승계하지 말고 영향받는 lane을 새 SHA에서
  다시 실행해.

  scenario 완료를 compile 성공이나 runner exit 0만으로 판단하지 마.

  다음 조건을 모두 만족해야 한다.

  - exact scenario selector가 다섯 언어에서 실제로 dispatch됨
  - 다섯 언어의 실제 역할 process가 실행됨
  - client-visible 결과가 계약과 일치함
  - 역할 server evidence가 계약과 일치함
  - 기대 실패와 timeout 결과가 검증됨
  - child process와 외부 자원이 정리됨
  - exit status와 원본 log가 보존됨
  - source/package/process evidence가 구분됨
  - 동일한 target SHA와 package provenance가 확인됨
  - 필요한 cross-language 조합이 완료됨
  - 미확인 skip이나 fixture 대체가 없음

  wire, codec, routing, protocol representation 또는 producer/consumer 역할처럼 언어가
  섞이는 동작은 구현 전에 reviewer가 cross-language matrix를 정해.

  matrix는 모든 관련 언어와 서로 다른 wire 역할을 포함해야 하며, 어떤 조합을 생략하면
  동일한 contract 경로로 대체된다는 근거를 ledger에 기록해. 편의상 임의로 조합을 줄이지 마.

  각 scenario가 끝날 때 coordinator가 다섯 언어와 cross-language evidence를 직접 검토하고
  `APPROVED`, `REJECTED` 또는 `REOPENED`를 판정해. `APPROVED` 전에는 다음 scenario로
  넘어가지 마.

  ## 10. durable evidence ledger

  repo 정책에 맞는 untracked run directory에 durable scenario ledger와 원본 log를 유지해.

  ledger에는 최소한 다음을 기록해.

  - `BASE_SHA`, 현재 candidate SHA와 최종 SHA
  - 전체 scenario inventory와 ID
  - scenario 상태
  - reviewer verdict
  - common spec과 exact interface의 `file:line`
  - 다섯 언어별 source/package/process 상태
  - 실행한 정확한 command와 exit code
  - package source SHA와 provenance
  - client-visible 결과
  - 역할 server evidence와 원본 log 위치
  - cleanup 확인 결과
  - cross-language matrix와 결과
  - production 변경 및 owner regression
  - coordinator의 최종 승인 또는 거절
  - blocker와 reopen 사유

  컨텍스트가 압축되거나 작업이 재개되면 repository 상태와 ledger를 먼저 대조하고,
  이미 완료한 작업을 추측으로 반복하거나 미완료 항목을 완료로 간주하지 마.

  raw log, package archive, build output와 run directory는 evidence로 보존할 수 있지만
  commit하지 마.

  ## 11. 최종 aggregate와 독립 리뷰

  모든 scenario 구현이 끝나면 최종 candidate를 `FINAL_SHA`로 고정해.

  `FINAL_SHA`에서 다섯 언어 package를 새로 생성하고 isolated cache와 clean consumer를
  사용해 다음을 수행해.

  - 언어별 전체 aggregate E2E
  - 전체 scenario inventory와 실제 selector dispatch 대조
  - 다섯 언어 package provenance 검증
  - 필요한 전체 cross-language matrix
  - resource leak와 잔여 process 확인
  - flaky 재현 여부 확인
  - 최종 contract-to-evidence 리뷰

  scenario checkpoint의 과거 성공은 `FINAL_SHA`의 aggregate 결과를 대체하지 않아.
  final aggregate에서 실패하거나 production이 변경되면 영향받는 scenario를 다시 열고
  수정·재검증해.

  최종 독립 integration reviewer가 `FINAL_SHA`의 ledger, diff, package provenance,
  aggregate 결과와 cross-language evidence를 read-only로 검토하게 해.

  coordinator는 독립 reviewer의 결과를 근거로 최종 승인 또는 거절을 직접 판정해.

  다음 중 하나라도 남으면 `CLEAN`을 선언하지 마.

  - 미구현 scenario
  - 실제로 dispatch되지 않은 selector
  - skip 또는 임의의 `N/A`
  - fixture gap
  - production gap
  - package provenance 불일치
  - cross-language evidence 누락
  - flaky
  - resource leak
  - child process 잔존
  - cleanup 미검증
  - 원본 log 또는 exit-status 누락
  - contract 해석 문제
  - final SHA와 다른 SHA의 evidence

  ## 12. commit과 `main` push

  검증된 checkpoint는 관련 path만 commit해서 `main`에 push해.

  commit 단위와 시점은 review, bisect와 재실행이 쉬운 방향으로 coordinator가 결정해.
  language agent나 reviewer는 직접 `main`에 push하지 마.

  commit 전에 다음을 확인해.

  - unrelated dirty change가 staging되지 않음
  - build artifact, package archive, 실행 log와 run directory가 staging되지 않음
  - protected spec/internals 문서가 변경되지 않음
  - staged path가 해당 checkpoint 범위와 일치함
  - staged diff와 `git diff --check`가 정상임
  - commit에 기록할 검증 명령과 결과가 ledger와 일치함

  push 직전에 `git fetch origin`으로 `origin/main` 이동 여부를 확인해. non-fast-forward이면
  force push하지 말고 upstream 변경을 통합한 뒤 영향받는 검증을 다시 실행해.

  push 후에는 local HEAD와 `origin/main`의 SHA 일치 여부, divergence와 작업공간 상태를
  확인해.

  branch protection, 권한 또는 외부 상태 때문에 `main` push가 불가능하면 이를 우회하거나
  다른 branch를 최종 결과로 간주하지 말고 정확한 상태를 blocker로 보고해.

  ## 13. 진행 보고와 중단 조건

  진행 중에는 scenario별로 다음만 간결하게 보고해.

  - scenario ID와 목적
  - reviewer verdict
  - 다섯 언어 상태
  - cross-language 상태
  - candidate SHA
  - 현재 blocker 또는 다음 단계

  사소한 구현 판단, language-specific private 구조, 안전한 진단과 in-scope test 실행은
  스스로 결정하고 계속 진행해. 일반적인 implementation gap이나 test failure는 고치고
  재검증해야 하며, 단순히 어렵거나 오래 걸린다는 이유로 blocker로 처리하지 마.

  다음 판단이 필요한 경우에만 구현을 멈추고 보고해.

  - common spec 사이의 사용자 관찰 동작 충돌
  - common spec과 exact interface의 해석 충돌
  - spec에 없는 새 public 동작
  - 새 public API 필요
  - protected spec/internals 변경 필요
  - Core 또는 bindings 변경 필요
  - 현재 Framework E2E 범위를 실질적으로 확장해야 함
  - 안전한 조사와 대안을 모두 소진해도 외부 권한이나 상태 때문에 진행할 수 없음

  중단 보고에는 반드시 다음을 포함해.

  - 정확한 `file:line`
  - 충돌하거나 비어 있는 계약 문구
  - 다섯 언어별 영향
  - 현재 source와 process evidence
  - 가능한 선택지와 trade-off
  - 이미 변경한 관련 파일의 유무
  - blocker가 해소되면 다시 열어야 할 scenario

  모든 scenario와 최종 검토가 `CLEAN`이 되거나 위 중단 조건에 해당하는 실제 blocker가
  확인될 때까지 Goal을 계속 수행해.