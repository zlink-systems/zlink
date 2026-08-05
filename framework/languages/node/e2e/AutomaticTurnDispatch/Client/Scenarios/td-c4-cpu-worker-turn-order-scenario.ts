// TD-C4: CPU worker와 terminator 역할을 분리한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdC4 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdC4();
