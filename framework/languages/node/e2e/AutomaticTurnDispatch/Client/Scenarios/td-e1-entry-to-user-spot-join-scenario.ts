// TD-E1: Entry Spot Actor의 deferred Join을 handler 뒤 실행한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdE1 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdE1();
