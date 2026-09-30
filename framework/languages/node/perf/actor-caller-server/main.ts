import { readConfig } from '../server-support/server-application';
import { runActorNoBindRequestEcho } from './actor-no-bind-request-echo-scenario';
import { runActorNoBindSendSendEcho } from './actor-no-bind-send-send-echo-scenario';

const { config } = readConfig(process.argv.slice(2));
if (config.role !== 'actor-caller' || !config.source) throw new Error('ActorCallerServer runs the source role of §10.9 and §10.10.');
const run = config.scenario === 'actor-no-bind-request-echo' ? () => runActorNoBindRequestEcho(config)
  : config.scenario === 'actor-no-bind-send-send-echo' ? () => runActorNoBindSendSendEcho(config)
    : undefined;
if (!run) throw new Error(`ActorCallerServer does not run scenario '${config.scenario}'.`);
run().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
