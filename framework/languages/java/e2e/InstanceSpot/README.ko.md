# Java Instance Spot E2E fixture

이 fixture는 Redis Location Store를 사용하는 owner 2개와 caller 2개를 process로 실행한다.
caller는 `ZLinkRouteClient`의 public request/send API를 사용하고, owner는
`ZLinkInstanceSpot` factory와 typed handler evidence endpoint를 제공한다. runner는 내부
Location Store row나 raw frame을 읽지 않는다.

```bash
cd framework/languages/java/e2e/InstanceSpot
./run_e2e.sh IS-E2E-01
./run_e2e.sh IS-E2E-03
./run_e2e.sh all
```

각 실행은 `logs/<run-id>/scenario-status.tsv`와 scenario별 process/API/evidence 파일을
생성한다. PASS는 실제 assertion 통과이며, BLOCKED는 실행 evidence와 함께 정확한 원인을
기록하고 exit code `3`을 반환한다. `all`은 BLOCKED를 성공으로 바꾸지 않으므로 모든
scenario가 PASS가 아닌 경우 non-zero를 반환한다.

Gradle distribution이 없으면 runner가 `Owner:installDist`와 `Client:installDist`를 실행한다.
이미 존재하는 distribution은 재사용할 수 있다. Core/runtime public API는 이 fixture에서
변경하지 않는다.
