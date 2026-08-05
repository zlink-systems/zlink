// TD-A5: Async 대기 중 due timer는 handler 뒤 실행된다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdA5 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdA5();
