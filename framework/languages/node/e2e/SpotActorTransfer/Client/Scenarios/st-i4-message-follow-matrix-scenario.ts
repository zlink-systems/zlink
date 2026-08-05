// ST-I4: Message Follow authority boundaries 시나리오를 검증한다.
import { runStF4 } from './st-f4-bound-session-transfer-scenario';

export async function runStI4(): Promise<void> {
  // 실제 transport delivery를 이전 owner에 고정한 뒤 commit 후 one-way와
  // request를 전달해 Actor Message Follow의 positive·expiry 경계를 검증한다.
  await runStF4('ST-I4');
}
