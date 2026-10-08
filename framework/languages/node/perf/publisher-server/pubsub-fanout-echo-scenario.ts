import * as path from 'node:path';
import type { ZLinkFanoutClient, ZLinkFrameworkRuntime } from '@zlink-systems/framework';
import { ZLINK_FANOUT_CLIENT, ZLINK_FRAMEWORK_RUNTIME } from '@zlink-systems/nestjs';
import { PerfClock } from '../shared/clock';
import { DecimalText, PerfPublishEvent, RoleConfig } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import {
  FanoutMetrics,
  PublisherSequences,
  SequenceBitSet
} from '../server-support/fanout-metrics';
import { ObjectsReadiness, runRole } from '../server-support/server-application';
import { runAdmissionStreams, until } from '../server-support/wait';

const PUBLISHER_NO_DROP = true;

// §10.11 Publisher: one process issues every sequence of the run. Each logical stream awaits the public publish
// admission (ZLinkFanoutClient.publish(...).submit()); nothing waits for a subscriber. Delivery is not observed here:
// the Subscribers' own originals are intersected with this process's window-success set by the runner (§15.4).
export class PubSubFanoutEchoScenario {
  private issued = 0; // run-wide: warmup and measured ranges never overlap
  private measuredBase = 0; // `issued` when the measured epoch was reset
  private windowSuccesses = new SequenceBitSet();
  private readonly sequenceFile: string;

  constructor(
    private readonly fanout: ZLinkFanoutClient,
    private readonly runtime: ZLinkFrameworkRuntime,
    private readonly measurement: Measurement,
    private readonly config: RoleConfig,
    private readonly objects: ObjectsReadiness,
    cellDirectory: string
  ) {
    this.sequenceFile = path.join(cellDirectory, 'publisher-sequences.json');
    measurement.onReset = () => {
      this.measuredBase = this.issued;
      this.windowSuccesses = new SequenceBitSet();
    };
    measurement.messageTypes = [{ direction: 'event', packetName: 'PerfPublishEvent' }];
    measurement.enrichSnapshot = (snapshot) => this.enrich(snapshot);
  }

  // The Publisher has no subscriber-facing status: its host Ready plus the Subscribers' public Ready
  // (fanout runtime status, one per Subscriber process) is the prepared state.
  async prepare(): Promise<void> {
    try {
      await until(
        () => this.runtime.status.isReady,
        this.config.workload.setupTimeoutMs,
        'the Publisher host to be ready'
      );
      const status = this.runtime.status;
      this.objects.set(true, '', [
        {
          kind: 'publisherHostReady',
          source: 'ZLinkFrameworkRuntime.status',
          observedValue: {
            state: status.state,
            isReady: status.isReady,
            acceptingWork: status.acceptingWork
          }
        }
      ]);
    } catch (error) {
      this.measurement.recordDiagnostic(error);
    }
  }

  run = (): Promise<void> =>
    runAdmissionStreams(
      this.config.workload.logicalStreams as number,
      () => this.measurement.canIssue,
      () => this.loop()
    );

  private async loop(): Promise<boolean> {
    const { config, measurement } = this;
    const started = measurement.beginOperation('event');
    if (started === undefined) return false;
    const sequence = ++this.issued;
    const warmup = measurement.resetSeq === '0';
    const message = new PerfPublishEvent({
      runId: config.runId,
      cellId: config.cellId,
      resetSeq: measurement.resetSeq,
      phase: warmup ? 'warmup' : 'measured',
      sequence: String(sequence),
      topic: FanoutMetrics.topic,
      sentTicks: DecimalText.of(started),
      clockDomainId: PerfClock.domain,
      payload: measurement.pattern.base64
    });
    try {
      await this.fanout.publish(config.channelName!, FanoutMetrics.topic, message).submit();
      const completed = PerfClock.now();
      const windowSuccess = measurement.completeOperation(started, undefined, completed);
      if (warmup) {
        if (measurement.setupEvidence.length === 0)
          measurement.setupEvidence = [
            {
              kind: 'warmupMarkerPublished',
              source: 'ZLinkFanoutClient.publish.submit',
              observedValue: message.sequence
            }
          ];
      } else if (windowSuccess) this.windowSuccesses.trySet(sequence);
    } catch (error) {
      measurement.completeOperation(started, error);
    }
    return true;
  }

  private enrich(snapshot: Parameters<NonNullable<Measurement['enrichSnapshot']>>[0]): void {
    FanoutMetrics.applyCommon(snapshot, false);
    FanoutMetrics.value(snapshot, 'messages.publishedInWindow', String(this.windowSuccesses.count));
    const seconds = snapshot.window.measuredSeconds;
    if (seconds !== null && seconds > 0)
      FanoutMetrics.value(
        snapshot,
        'fanout.publishOpsPerSec',
        this.windowSuccesses.count / seconds
      );
    else
      FanoutMetrics.setNull(
        snapshot,
        'fanout.publishOpsPerSec',
        'PHASE_NOT_STARTED',
        'No measured window has run.'
      );
    snapshot.provenance.fanout = {
      channelName: this.config.channelName,
      topic: FanoutMetrics.topic,
      noDrop: PUBLISHER_NO_DROP,
      publisherSequenceScope: 'one counter per run; warmup and measured ranges are disjoint',
      sequenceOriginal: 'publisher-sequences.json'
    };
    if (
      !this.measurement.finalSnapshot ||
      snapshot.phase !== 'complete' ||
      snapshot.resetSeq !== '1'
    )
      return;
    const last = this.issued;
    const original: PublisherSequences = {
      runId: this.config.runId,
      cellId: this.config.cellId,
      resetSeq: snapshot.resetSeq,
      phase: 'measured',
      attemptedRanges:
        last > this.measuredBase
          ? [{ first: String(this.measuredBase + 1), last: String(last) }]
          : [],
      windowSuccessRanges: this.windowSuccesses.ranges()
    };
    FanoutMetrics.writeOnce(this.sequenceFile, original);
  }
}

export async function runPubSubFanoutEcho(
  config: RoleConfig,
  cellDirectory: string
): Promise<void> {
  const measurement = new Measurement(config, config.source);
  const readiness = new ObjectsReadiness(false, 'The Publisher host is not Ready yet.');
  let scenario: PubSubFanoutEchoScenario | undefined;
  // Automatic Classic fanout: the publisher listens on the reserved endpoint and publishes its descriptor to the run's Store.
  await runRole(
    {
      config,
      objects: readiness,
      providers: [],
      configureFramework: (builder) => {
        builder
          .addFanoutChannel(config.channelName!)
          .enablePublisher(config.transportEndpoints.fanout)
          .setAdvertiseHost('127.0.0.1')
          .setRoutingIdPrefix('perf-publisher')
          .setNoDrop(PUBLISHER_NO_DROP)
          .setSendTimeout(config.workload.socketSendTimeoutMs);
      },
      workload: () => scenario?.run,
      prepare: async (app) => {
        scenario = new PubSubFanoutEchoScenario(
          app.get<ZLinkFanoutClient>(ZLINK_FANOUT_CLIENT, { strict: false }),
          app.get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME, { strict: false }),
          measurement,
          config,
          readiness,
          cellDirectory
        );
        await scenario.prepare();
      }
    },
    measurement
  );
}
