import { readConfig } from '../server-support/server-application';
import { runPubSubFanoutEcho } from './pubsub-fanout-echo-scenario';

const { config, cellDirectory } = readConfig(process.argv.slice(2));
if (config.scenario !== 'pubsub-fanout-echo' || config.role !== 'publisher' || !config.source) {
  throw new Error('PublisherServer runs the publisher role of pubsub-fanout-echo.');
}
runPubSubFanoutEcho(config, cellDirectory).catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
