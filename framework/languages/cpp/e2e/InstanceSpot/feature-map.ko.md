# Config 14 C++ E2E feature map

공통 Config 14 selector에는 36개 scenario가 등록되어 있다. 현재 C++ process
fixture가 실제 절차와 evidence를 구현한 scenario만 `implemented`로 표시한다.
나머지는 client가 즉시 실패하므로 정상 request로 실행되어 PASS로 집계되지 않는다.

| ID | 상태 | 근거 |
|---|---|---|
| IS-E2E-02 | implemented | one-way send와 handler evidence를 검증한다. |
| IS-E2E-03 | implemented | 동일 Instance Spot으로 보낸 병렬 request를 검증한다. |
| IS-E2E-17 | implemented | 서로 다른 Instance Spot의 병렬 request를 검증한다. |
| IS-E2E-05 | implemented | 두 owner가 같은 type을 제공한 상태에서 Ready Spot owner를 `SIGKILL`로 종료한다. Owner lease 만료 뒤 후속 request가 `Unavailable`로 끝나며 양쪽 handler evidence가 없고 surviving owner가 새 instance를 만들지 않았는지 확인한다. |
| 그 외 | blocked | 공통 문서의 scenario별 절차와 visible assertion이 구현되지 않았다. |

`blocked`는 구현 완료나 PASS가 아니다. 각 scenario의 실제 process 절차와 실패
관찰 지점을 추가한 뒤에만 상태를 변경한다.
