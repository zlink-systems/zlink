// TD-D5: 지원하지 않는 문맥의 Yield를 operation 제출 전에 거부한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';

export const runTdD5 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdD5();
