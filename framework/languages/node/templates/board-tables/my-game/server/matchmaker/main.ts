import {
  ZLinkSpotCreateState,
  ZLinkUserSpotExecutionMode,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { Names } from '../shared/constants';
import { addGameMesh, failHost, runHost } from '../shared/hosting';
import {
  CancelPlayer,
  ConfirmWithdrawal,
  QueuePlayer,
  ReportEntry,
  ReportReturn,
  UpdatePresence
} from './handlers';
import { Pool, PoolRelocation } from './pool';

runHost((builder, config) => {
  const mesh = addGameMesh(builder, config);
  mesh
    .objects()
    .server()
    .addSpotFactory(Names.pool, Pool, (factory) => {
      factory.executionMode(ZLinkUserSpotExecutionMode.SpotWide);
      factory.preserveStateWith(PoolRelocation);
    });
  return {
    providers: [
      Pool,
      PoolRelocation,
      QueuePlayer,
      CancelPlayer,
      UpdatePresence,
      ConfirmWithdrawal,
      ReportEntry,
      ReportReturn
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable,
    async initialize(app) {
      const created = await app
        .get<ZLinkSpotManager>(ZLINK_SPOT_MANAGER)
        .getOrCreate(Names.poolKey, Names.pool)
        .inMesh(Names.mesh)
        .submit();
      if (created.state === ZLinkSpotCreateState.Rejected)
        throw new Error('Pool creation rejected');
    }
  };
}).catch(failHost);
