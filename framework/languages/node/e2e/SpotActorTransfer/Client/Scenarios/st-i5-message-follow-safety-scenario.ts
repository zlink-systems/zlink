// ST-I5: Message Follow error bounds 시나리오를 검증한다.
import { runStF4 } from './st-f4-bound-session-transfer-scenario';

export async function runStI5(): Promise<void> {
  // 같은 이전-route delivery를 두 번 제출해 exactly-once를 검증하고,
  // duration 만료 뒤에는 stale-route terminal과 handler 미실행을 검증한다.
  await runStF4('ST-I5');
}
