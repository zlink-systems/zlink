import { Names } from '../shared/constants';
import { failHost, runHost } from '../shared/hosting';
import { CommitRoundHandler, LoadPlayerHandler, RedisPlayerRecords } from './player-records';

runHost((builder, config) => {
  if (!config.apiBindHost) throw new Error('API requires apiBindHost');
  builder
    .addClientServerChannel(Names.playerRecords)
    .server()
    .setBindHost(config.apiBindHost)
    .setAdvertiseHost(config.apiAdvertiseHost ?? config.apiBindHost)
    .listen()
    .addHandlerGroup(Names.playerRecords);
  return {
    providers: [RedisPlayerRecords, LoadPlayerHandler, CommitRoundHandler],
    listener: { kind: 'clientServer', name: Names.playerRecords }
  };
}).catch(failHost);
