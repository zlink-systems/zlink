import { Names } from '../shared/constants';
import { addGameMesh, failHost, runHost } from '../shared/hosting';
import {
  CancelPlayer,
  ConfirmWithdrawal,
  QueuePlayer,
  ReportEntry,
  ReportPreparation,
  ReportRoomClosure,
  ReportReturn,
  UpdatePresence
} from './handlers';
import { Pool, PoolRelocation } from './pool';

runHost((builder, config) => {
  const mesh = addGameMesh(builder, config);
  mesh
    .objects()
    .server()
    .addInstanceSpotFactory(Names.pool, Pool, (factory) => {
      factory.stableTypeLimit(1);
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
      ReportPreparation,
      ReportRoomClosure,
      ReportReturn
    ],
    listener: { kind: 'routeMesh', name: Names.mesh },
    ready: (status) => status.placement.isAvailable
  };
}).catch(failHost);
