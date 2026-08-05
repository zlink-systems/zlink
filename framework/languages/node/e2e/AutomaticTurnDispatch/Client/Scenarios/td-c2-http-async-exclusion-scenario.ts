// TD-C2: I/O worker를 Async로 기다리면 turn을 유지한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdC2 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdC2();
