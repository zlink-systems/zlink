import { NullReason, nullReason } from './contracts';
import { LATENCY_SUFFIXES } from './histogram';

// The §14 keys every snapshot carries. A scenario fills the ones it measures; the rest stay null with a reason.
export const MetricCatalog = {
  auxiliaryPrefixes: ['actor.sourceAdmission.latency', 'spot.remoteCallLatency', 'driver.latency', 'worker.callLatency',
    'worker.submitToStart', 'worker.taskLatency', 'worker.resultToContinuation', 'fanout.deliveryLatency', 'fanout.settleDeliveryLatency'],
  auxiliaryHistograms: ['sourceAdmissionMs', 'driverLatencyMs', 'workerCallLatencyMs', 'workerSubmitToStartMs', 'workerTaskLatencyMs',
    'workerResultToContinuationMs', 'fanoutDeliveryLatencyMs', 'fanoutSettleDeliveryLatencyMs'],
  inapplicable: ['messages.admitted', 'messages.expired', 'messages.duplicateReply', 'messages.lateReply', 'messages.unknownCorrelation',
    'spot.applicationYieldCalls', 'spot.applicationHandlerEntries', 'driver.issued', 'driver.notStarted', 'driver.failed',
    'messages.published', 'messages.publishedInWindow', 'messages.settlePublished', 'fanout.subscriberCount', 'fanout.uniqueDelivered',
    'fanout.deliveredInWindow', 'fanout.settleDelivered', 'fanout.duplicateEvents', 'fanout.outOfCohortEvents', 'fanout.deliveryRatio',
    'fanout.publishOpsPerSec', 'fanout.deliveryOpsPerSec', 'spot.mailboxDepth.max', 'spot.mailboxDepth.mean', 'spot.suspendedTurns',
    'spot.resumedTurns', 'spot.resumeLatency.p95Ms', 'spot.resumeLatency.p99Ms', 'worker.pool.queueDepth.max', 'worker.pool.queueDepth.mean'],
  outcomes: ['sent', 'completed', 'settleCompleted', 'failed', 'timeout', 'cancelled', 'unresolved'] as const,

  setNull(values: Record<string, unknown>, reasons: Record<string, NullReason>, container: string, key: string, code: string, reason: string): void {
    values[key] = null;
    reasons[`/${container}/${key}`] = nullReason(code, reason);
  },

  baselineNulls(metrics: Record<string, unknown>, histograms: Record<string, unknown>, reasons: Record<string, NullReason>): void {
    const keys = [...MetricCatalog.inapplicable, ...MetricCatalog.auxiliaryPrefixes.flatMap((prefix) => LATENCY_SUFFIXES.map((suffix) => `${prefix}.${suffix}`))];
    for (const key of keys) MetricCatalog.setNull(metrics, reasons, 'metrics', key, 'NOT_APPLICABLE', 'The phase 1 request baseline has no corresponding operation.');
    for (const key of MetricCatalog.auxiliaryHistograms)
      MetricCatalog.setNull(histograms, reasons, 'histograms', key, 'NOT_APPLICABLE', 'The request baseline does not measure this interval.');
    for (const suffix of ['p50Ms', 'p95Ms', 'p99Ms'])
      MetricCatalog.setNull(metrics, reasons, 'metrics', `host.queueWaitLatency.${suffix}`, 'PUBLIC_OBSERVATION_UNSUPPORTED',
        'Public status provides no exact pre-receive to handler queue-wait hook.');
  }
};
