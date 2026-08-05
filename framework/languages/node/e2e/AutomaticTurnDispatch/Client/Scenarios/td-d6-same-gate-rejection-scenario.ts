// TD-D6: 같은 claim이 필요한 awaited request를 거부한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';

export const runTdD6 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdD6();
