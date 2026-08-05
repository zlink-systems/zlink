// TD-C5: CPU worker saturation이 I/O worker를 막지 않는다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdC5 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdC5();
