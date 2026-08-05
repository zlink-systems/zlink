// TD-A1: One-way send 완료는 handler 완료를 기다리지 않는다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdA1 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdA1();
