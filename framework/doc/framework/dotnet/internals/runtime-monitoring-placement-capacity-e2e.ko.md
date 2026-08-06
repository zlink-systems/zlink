# Runtime monitoring placement capacity E2E

이 문서는 .NET Framework의 RuntimeMonitoring E2E가 public placement 상태와
실제 object lifecycle 결과를 대조하는 방법을 설명한다. 공통 E2E의 MON-A6
시나리오를 구현하기 위한 유지보수자용 기록이며, Framework public contract를
추가하지 않는다.

## 검증 topology

MON-A6는 작은 capacity를 가진 단일 service node에서 실행한다. 다른 service
node를 함께 시작하면 placement selector가 remote node를 선택할 수 있으므로,
호출한 node의 placement snapshot과 create 결과를 일대일로 대조할 수 없다.

service node는 actor와 spot limit을 각각 1로 설정한다. E2E client는 HTTP를
통해 public actor·spot manager API와 `IZLinkRouteMeshRuntime` snapshot을
호출한다. Framework 내부 registry나 reservation counter는 판정에 사용하지
않는다.

## 판정 순서

1. 초기 placement snapshot의 active actor·spot count가 0인지 확인한다.
2. actor와 User Spot을 각각 하나 생성하고 count가 1로 증가하는지 확인한다.
3. 같은 종류의 추가 생성이 capacity failure로 끝나는지 확인한다.
4. 기존 actor와 spot을 close하고 count가 0으로 돌아오는지 확인한다.
5. 새 object를 다시 생성하고 count와 `IsAvailable`이 복구되는지 확인한다.

count는 lifecycle API가 반환한 성공 결과의 증거이며, snapshot은 그 결과가
public 운영 상태에 반영된 증거다. 두 결과가 일치하지 않으면 runner의
대기 시간을 늘려 통과시키지 않고 placement ownership 또는 status projection
구현을 다시 조사해야 한다.

실행 증거는
`framework/languages/dotnet/e2e/RuntimeMonitoring/logs/20260806-194135-923017`
에 보존한다.
