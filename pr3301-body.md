Closes #3301

**0.31.0 태그 뒤에 큐에 넣습니다** (C++ 소스 아카이브 해시 고정, #3286).

## 원인
`test_cpp_framework_local_enqueue_allocations`는 warm-up request의 reply 완료를 infrastructure 작업 완료로 간주했습니다. request 등록은 deadline 인덱스 갱신을 소유 lane에 따로 post하므로(`operation_registry.cpp:382`), 그 작업이 남아 있으면 다음 제출이 즉시 ready가 아니어서 `pending.await_ready()` 단언이 실패했습니다. 분류: 시험 동기화 결함, 제품 코드 변경 없음.

## 수정
측정 전에 기존 완료 신호 `infrastructure_executor()->drained()`(queue와 active 작업 수를 함께 확인)를 기존과 같은 5초 범위 안에서 관찰한 뒤 할당을 잽니다. 단언·sleep·재시도·제출 순서는 바꾸지 않았습니다.

## 검증
- 부하(`taskset -c 8-15`, `stress-ng --cpu 8`)에서 시험 50회: 수정 전 17/50 실패 → 수정 후 0/50
- 역방향 확인: 측정 구간에 허용 상한+1바이트 할당을 임시로 넣으면 1/1 실패(`frame_allocations == 0` 단언이 검출), 임시 코드는 제거

### 로컬 전체 시험 (pre-pr.sh, HEAD ec4c1f282d, main e40bff6c43 병합)
| 단계 | 결과 |
|---|---|
| C++ Release 빌드 | 통과 |
| ctest unit·contract | 268/268 통과 |
| format.sh --check cpp | 통과 |

🤖 Generated with [Claude Code](https://claude.com/claude-code)
