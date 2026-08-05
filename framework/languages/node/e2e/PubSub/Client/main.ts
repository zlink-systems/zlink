import { parseClientOptions } from './Support/client-options';
import { ServerProcessLauncher, type DynamicProcess } from './Support/server-process-launcher';
import { runPsA1 } from './Scenarios/ps-a1-fanout-basic-delivery-scenario';
import { runPsA2 } from './Scenarios/ps-a2-topic-filter-scenario';
import { runPsA3 } from './Scenarios/ps-a3-late-subscriber-scenario';
import { runPsA4 } from './Scenarios/ps-a4-subscriber-reconnect-scenario';
import { runPsB1 } from './Scenarios/ps-b1-slow-subscriber-scenario';
import { runPsB2 } from './Scenarios/ps-b2-publisher-restart-scenario';
import { runPsC1 } from './Scenarios/ps-c1-missing-message-name-scenario';
import { runPsE1 } from './Scenarios/ps-e1-manual-endpoint-without-store-scenario';
import { runPsF3 } from './Scenarios/ps-f3-reserved-liveness-topic-scenario';
import { runPsD1 } from './Scenarios/ps-d1-automatic-discovery-scenario';
import { runPsD3 } from './Scenarios/ps-d3-publisher-set-convergence-scenario';
import { runPsF4 } from './Scenarios/ps-f4-orderly-disconnect-scenario';
import { runPsD4 } from './Scenarios/ps-d4-crash-replacement-scenario';
import { runPsD5 } from './Scenarios/ps-d5-store-failure-static-connection-scenario';
import { runPsD6 } from './Scenarios/ps-d6-port-zero-restart-scenario';
import { runPsF5 } from './Scenarios/ps-f5-unsubscribed-traffic-liveness-scenario';
import { runPsF2 } from './Scenarios/ps-f2-publisher-liveness-isolation-scenario';
import { runPsD7A } from './Scenarios/ps-d7a-slow-observer-isolation-scenario';
import { runPsD2 } from './Scenarios/ps-d2-channel-name-filter-scenario';
import { runPsE2A } from './Scenarios/ps-e2a-automatic-subscriber-without-store-scenario';
import { runPsE2B } from './Scenarios/ps-e2b-mixed-subscriber-mode-scenario';
import { runPsE2C } from './Scenarios/ps-e2c-publisher-identity-validation-scenario';

async function main(): Promise<void> {
  const options = parseClientOptions(process.argv.slice(2));
  const processes = new ServerProcessLauncher(options);
  let restartedPublisher: DynamicProcess | undefined;
  const subscribers = options.subscriberUrls;

  const scenarios: Record<string, () => Promise<void>> = {
    'PS-A1': () => runPsA1(options.publisherUrl, subscribers),
    'PS-A2': () => runPsA2(options.publisherUrl, subscribers),
    'PS-A3': () => runPsA3(options.publisherUrl, options.lateSubscriberUrl, processes, options.publisherEndpoint),
    'PS-A4': () => runPsA4(options.publisherUrl, options.lateSubscriberUrl, subscribers.slice(0, 2), processes, options.publisherEndpoint),
    'PS-B1': () => runPsB1(options.publisherUrl, subscribers.slice(0, 2), subscribers[subscribers.length - 1]),
    'PS-B2': async () => { restartedPublisher = await runPsB2(options.publisherUrl, subscribers, processes); },
    'PS-C1': () => runPsC1(options.publisherUrl, subscribers),
    'PS-E1': () => runPsE1(
      options.publisherUrl,
      options.lateSubscriberUrl,
      processes,
      options.publisherEndpoint
    ),
    'PS-F3': () => runPsF3(options.publisherUrl, subscribers),
    'PS-D1': () => runPsD1(options.publisherUrl, subscribers[0]),
    'PS-D2': () => runPsD2(options.publisherUrl, options.secondaryPublisherUrl, subscribers[0]),
    'PS-D3': () => runPsD3(options.publisherUrl, options.secondaryPublisherUrl, subscribers[0], processes),
    'PS-F4': () => runPsF4(options.publisherUrl, options.secondaryPublisherUrl, subscribers[0], processes),
    'PS-D4': () => runPsD4(options.publisherUrl, options.secondaryPublisherUrl, subscribers[0], processes),
    'PS-D5': () => runPsD5(
      options.publisherUrl,
      subscribers[0],
      options.redisEndpoint,
      options.redisKeyPrefix,
      processes
    ),
    'PS-D6': () => runPsD6(options.publisherUrl, options.subscriberUrls[0], processes),
    'PS-F5': () => runPsF5(options.publisherUrl, subscribers[0]),
    'PS-F2': () => runPsF2(
      options.publisherUrl,
      options.secondaryPublisherUrl,
      subscribers[0],
      options.publisherProxyPort,
      processes
    ),
    'PS-D7A': () => runPsD7A(options.publisherUrl, options.secondaryPublisherUrl, subscribers[0], processes),
    'PS-E2A': () => runPsE2A(options.lateSubscriberUrl, processes),
    'PS-E2B': () => runPsE2B(options.lateSubscriberUrl, options.publisherEndpoint, processes),
    'PS-E2C': () => runPsE2C(
      options.publisherUrl,
      options.lateSubscriberUrl,
      options.publisherIdentityMissingEndpoint ?? '',
      options.publisherIdentityBothEndpoint ?? '',
      options.redisEndpoint,
      options.redisKeyPrefix,
      processes
    )
  };

  try {
    if (options.scenario.toLowerCase() === 'all') {
      for (const scenario of Object.values(scenarios)) {
        await scenario();
      }
    } else {
      const selected = scenarios[options.scenario.toUpperCase()];
      if (selected === undefined) {
        throw new Error(`Unknown scenario '${options.scenario}'.`);
      }
      await selected();
    }
  } finally {
    if (restartedPublisher !== undefined && !restartedPublisher.hasExited) {
      await restartedPublisher.stop();
    }
  }

  console.log('pubsub e2e result=passed');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
