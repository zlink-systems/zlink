// TD-C3: I/O 대기가 CPU worker capacity를 사용하지 않는다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdC3 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdC3();
