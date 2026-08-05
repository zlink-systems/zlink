// TD-B1: Yield 대기 중 같은 Spot의 callback을 실행한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdB1 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdB1();
