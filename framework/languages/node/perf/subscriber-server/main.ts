import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_FANOUT_RUNTIME } from '@zlink-systems/nestjs';
import type { ZLinkFanoutHandler, ZLinkFanoutRuntime, ZLinkPublishMessageContext } from '@zlink-systems/framework';
import { PerfClock } from '../shared/clock';
import { PerfPublishEvent } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { ObjectsReadiness, readConfig, runRole } from '../server-support/server-application';
import { FanoutReceipts } from './fanout-receipts';

const { config, cellDirectory } = readConfig(process.argv.slice(2));
if (config.scenario !== 'pubsub-fanout-echo' || config.role !== 'subscriber' || config.source) {
  throw new Error('SubscriberServer runs the subscriber role of pubsub-fanout-echo.');
}

// §10.11 Subscriber: the typed fanout handler validates each event and records its unique sequence.
@Injectable()
class PerfFanoutHandler implements ZLinkFanoutHandler<PerfPublishEvent> {
  constructor(@Inject(Measurement) private readonly measurement: Measurement, @Inject(FanoutReceipts) private readonly receipts: FanoutReceipts) {}

  async handle(message: PerfPublishEvent, _context: ZLinkPublishMessageContext): Promise<void> {
    const receivedTicks = PerfClock.now();
    this.measurement.handlerEnter();
    try {
      this.receipts.record(message, receivedTicks);
    } catch (error) {
      this.measurement.recordDiagnostic(error);
      throw error;
    } finally {
      this.measurement.handlerExit();
    }
  }
}

const measurement = new Measurement(config, config.source);
const readiness = new ObjectsReadiness(false, 'No Ready publisher is visible to this Subscriber yet.');
const receipts = new FanoutReceipts(measurement, config, readiness, cellDirectory);
runRole({
  config,
  objects: readiness,
  providers: [PerfFanoutHandler, { provide: FanoutReceipts, useValue: receipts }],
  // Automatic Classic fanout: no endpoint and no subscribe(topic), so this subscriber receives every topic (§10.11).
  configureFramework: (builder) => {
    builder.addFanoutChannel(config.channelName!).enableSubscriber().addPublishHandler('PerfPublishEvent', PerfFanoutHandler);
  },
  prepare: async (app) => {
    await receipts.prepare(app.get<ZLinkFanoutRuntime>(ZLINK_FANOUT_RUNTIME, { strict: false }));
  }
}, measurement).catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
