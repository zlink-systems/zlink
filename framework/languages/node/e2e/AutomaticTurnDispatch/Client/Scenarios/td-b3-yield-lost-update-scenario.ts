// TD-B3: Yield 뒤에는 shared state를 다시 확인한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdB3 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdB3();
