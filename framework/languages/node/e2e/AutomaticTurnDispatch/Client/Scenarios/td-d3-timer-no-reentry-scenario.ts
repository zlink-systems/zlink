// TD-D3: Timer overrun 중 callback을 겹쳐 실행하지 않는다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdD3 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdD3();
