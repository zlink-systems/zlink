import * as path from 'node:path';
import type { ZLinkFanoutRuntime } from '@zlink-systems/framework';
import { DecimalText, PerfPublishEvent, PerfValidationException, RoleConfig, nullReason } from '../shared/contracts';
import { Measurement, PerfMetricsSnapshot } from '../shared/measurement';
import { FanoutMetrics, SequenceBitSet, SubscriberSequences } from '../server-support/fanout-metrics';
import { ObjectsReadiness } from '../server-support/server-application';
import { until } from '../server-support/wait';

// §10.11 Subscriber evidence: which measured sequences this process received first inside its own window or settle
// (§15.4). It counts nothing the Publisher published; the runner intersects both originals.
class Round {
  readonly window = new SequenceBitSet();
  readonly settle = new SequenceBitSet();
  duplicates = 0;
  warmupEvents = 0;
  ignoredWarmupInMeasured = 0;
  outsideWindow = 0;
  sealed = false; // set when the runner collects the final snapshot: later events are missing deliveries
}

export class FanoutReceipts {
  private round = new Round();
  private readonly sequenceFile: string;

  constructor(
    private readonly measurement: Measurement, private readonly config: RoleConfig, private readonly objects: ObjectsReadiness, cellDirectory: string
  ) {
    this.sequenceFile = path.join(cellDirectory, `subscriber-${config.roleInstance}-sequences.json`);
    measurement.onReset = () => { this.round = new Round(); };
    measurement.messageTypes = [{ direction: 'event', packetName: 'PerfPublishEvent' }];
    measurement.enrichSnapshot = (snapshot) => this.enrich(snapshot);
  }

  // objectsReady: this Subscriber's public fanout status shows a Ready publisher (§16.1).
  async prepare(fanoutRuntime: ZLinkFanoutRuntime): Promise<void> {
    try {
      let status = fanoutRuntime.snapshot(this.config.channelName!);
      await until(() => { status = fanoutRuntime.snapshot(this.config.channelName!); return status.isReady && status.readyPublisherCount > 0; },
        this.config.workload.setupTimeoutMs, 'a Ready fanout publisher');
      this.objects.set(true, '', [{ kind: 'fanoutStatus', source: 'ZLinkFanoutRuntime.snapshot', observedValue: status }]);
    } catch (error) {
      this.measurement.recordDiagnostic(error);
    }
  }

  record(message: PerfPublishEvent): void {
    const { config, measurement } = this;
    const current = this.round;
    if (message.runId !== config.runId || message.cellId !== config.cellId || message.topic !== FanoutMetrics.topic ||
      (message.phase !== 'warmup' && message.phase !== 'measured') || !message.clockDomainId)
      throw new PerfValidationException('IdentityMismatch', 'Fanout event identity does not match the cell.');
    const sequence = Number(DecimalText.u64(message.sequence));
    DecimalText.i64(message.sentTicks);
    measurement.pattern.validate(message.payload);
    if (message.phase === 'warmup') {
      if (DecimalText.u64(message.resetSeq) !== 0n) throw new PerfValidationException('PhaseMismatch', 'Warmup event carries a measured resetSeq.');
      current.warmupEvents++;
      if (measurement.setupEvidence.length === 0) measurement.setupEvidence = [{ kind: 'warmupMarker', source: 'ZLinkFanoutHandler<PerfPublishEvent>', observedValue: message.sequence }];
      if (measurement.resetSeq !== '0') current.ignoredWarmupInMeasured++;
      return;
    }
    if (message.resetSeq !== measurement.resetSeq) throw new PerfValidationException('PhaseMismatch', 'Measured event resetSeq differs from this epoch.');
    // The runner ends the settle (§4.1): receipts after the window are settle until it reads the final snapshot.
    const target = current.sealed ? undefined : measurement.phase === 'measured' ? current.window : measurement.phase === 'settle' || measurement.phase === 'complete' ? current.settle : undefined;
    if (target === undefined) current.outsideWindow++;
    else if (current.window.contains(sequence) || current.settle.contains(sequence) || !target.trySet(sequence)) current.duplicates++;
  }

  private enrich(snapshot: PerfMetricsSnapshot): void {
    const current = this.round;
    FanoutMetrics.applyCommon(snapshot, true);
    FanoutMetrics.value(snapshot, 'fanout.duplicateEvents', String(current.duplicates));
    snapshot.runtimeMetrics.fanoutReceipts = { name: 'subscriber receipts', unit: 'event', type: 'object', value: {
      uniqueInWindow: String(current.window.count), uniqueInSettle: String(current.settle.count), warmupEvents: String(current.warmupEvents),
      warmupInMeasuredEpoch: String(current.ignoredWarmupInMeasured), measuredOutsideWindow: String(current.outsideWindow) } };
    snapshot.provenance.fanout = { channelName: this.config.channelName, topic: FanoutMetrics.topic, subscribedTopics: [],
      delivery: 'typed ZLinkFanoutHandler<PerfPublishEvent>',
      sequenceEvidence: { method: 'one bit per received sequence, window and settle sets', retainedBytes: String(current.window.retainedBytes + current.settle.retainedBytes),
        timingEvidence: 'not collected: no shared clock domain', original: `subscriber-${this.config.roleInstance}-sequences.json` } };
    if (!this.measurement.finalSnapshot || snapshot.phase !== 'complete' || snapshot.resetSeq !== '1') return;
    current.sealed = true;
    const original: SubscriberSequences = {
      runId: this.config.runId, cellId: this.config.cellId, resetSeq: snapshot.resetSeq, phase: 'measured', subscriberId: this.config.roleInstance,
      windowRanges: current.window.ranges(), settleRanges: current.settle.ranges(), duplicateEvents: String(current.duplicates),
      nullReasons: { '/timingEvidence': nullReason('CLOCK_DOMAIN_UNVERIFIED', 'Publisher and Subscriber use process-local monotonic clocks; no shared clock domain is verified (§15.2).') },
      timingEvidence: null
    };
    FanoutMetrics.writeOnce(this.sequenceFile, original);
  }
}
