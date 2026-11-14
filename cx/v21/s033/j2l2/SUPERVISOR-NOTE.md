
## 02:35 UTC — native writer는 별도 commit으로 분리하십시오 (PM 지시)
- 커밋을 둘로 나누십시오: (1) parser 2→1 + correlation HexFormat(+ 관련 시험), (2) native 최종 Message writer(heap→Message.from 제거)와 그 시험. (2)는 (1) 위에 쌓습니다.
- 이유: Node에서는 native 직접 쓰기가 할당은 줄였지만 처리량은 오히려 떨어졌습니다. Java FFM allocate·write 비용이 heap 복사보다 클 수 있어 감독자가 교정된 bench로 (1)만 vs (1)+(2)를 A/B 한 뒤 (2)를 올릴지 정합니다.
- report.md에 두 commit SHA와 각 commit의 할당·parse CPU 수치를 따로 적으십시오. report.md 맨 위의 감독자 중간 보고는 지우지 말고 아래에 이어 쓰십시오.

## 02:52 UTC — measure.lock 공유 취득 전에 gate를 통과하십시오 (brief 보완)
- 모든 `flock -s /root/jobs/fx/cx/measure.lock <cmd>`를 `/root/jobs/fx/cx/measure-shared.sh <cmd>`로 바꾸십시오(같은 공유 lock, gate 통과 포함).
- 이유: 측정(exclusive)이 기다리는 동안 새 공유 취득이 계속 겹치면 측정이 굶습니다. gate는 측정이 끝날 때까지 새 공유 취득만 잠시 세웁니다. 이미 잡은 lock은 그대로 두십시오.
