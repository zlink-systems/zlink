// TD-E2A: Handler 실패 시 등록한 Join을 폐기한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';

export const runTdE2A = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdE2A();
