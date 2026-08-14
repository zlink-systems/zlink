import {
  AutoHwmProfile,
  type AutoHwmProfileValue,
  type CoreHwmBudgetSnapshot
} from '@zlink-systems/zlink';
import {
  ZLinkApplicationJobQueueProfile,
  ZLinkCoreHwmProfile,
  type ZLinkApplicationJobQueueStatus,
  type ZLinkCoreHwmStatus,
  type ZLinkHostCapacityStatus
} from '../../contracts';
import type { ZLinkCoreHwmOptions } from '../../contracts/Configuration/RegistrationTypes';
import type { ApplicationJobQueue } from './application-job-queue';

export class HostCapacityStatusProjection {
  constructor(
    private readonly coreOptions: ZLinkCoreHwmOptions | undefined,
    private readonly applicationJobQueue: ApplicationJobQueue
  ) {}

  snapshot(core: CoreHwmBudgetSnapshot | undefined): ZLinkHostCapacityStatus {
    return Object.freeze({
      measurementEpoch: core?.measurementEpoch ?? 0n,
      coreHwm: projectCoreHwm(this.coreOptions, core),
      applicationJobQueue: projectApplicationJobQueue(this.applicationJobQueue)
    });
  }

  reset(resetCore: () => void): void {
    resetCore();
    this.applicationJobQueue.resetMetrics();
  }
}

function projectApplicationJobQueue(
  queue: ApplicationJobQueue
): ZLinkApplicationJobQueueStatus {
  const snapshot = queue.snapshot();
  return Object.freeze({
    ...snapshot,
    configuredProfile: snapshot.configuredProfile as ZLinkApplicationJobQueueProfile
  });
}

function projectCoreHwm(
  options: ZLinkCoreHwmOptions | undefined,
  snapshot: CoreHwmBudgetSnapshot | undefined
): ZLinkCoreHwmStatus {
  return Object.freeze({
    configuredMemoryLimitBytes: options?.memoryLimitBytes,
    configuredBudgetBytes: options?.budgetBytes,
    configuredProfile: projectCoreProfile(options?.profile),
    effectiveBudgetBytes: snapshot?.effectiveCoreBudgetBytes ?? 0n,
    totalAppliedHwmBytes: snapshot?.totalAppliedHwmBytes ?? 0n,
    coreQueueAccountedBytes: snapshot?.coreQueueAccountedBytes ?? 0n,
    applicationAccountedBytes: snapshot?.applicationAccountedBytes ?? 0n,
    currentAccountedBytes: snapshot?.currentAccountedBytes ?? 0n,
    provisionalAccountedBytes: snapshot?.provisionalAccountedBytes ?? 0n,
    peakAccountedBytes: snapshot?.peakAccountedBytes ?? 0n,
    completionCurrentAccountedBytes: snapshot?.completionCurrentAccountedBytes ?? 0n,
    completionPeakAccountedBytes: snapshot?.completionPeakAccountedBytes ?? 0n,
    completionPendingMessageCount: snapshot?.completionPendingMessageCount ?? 0n,
    totalMessagingAccountedBytes: snapshot?.totalMessagingAccountedBytes ?? 0n,
    monitorQueueAppliedHwmBytes: snapshot?.monitorQueueAppliedHwmBytes ?? 0n,
    monitorQueueAccountedBytes: snapshot?.monitorQueueAccountedBytes ?? 0n,
    totalInstanceAppliedHwmBytes: snapshot?.totalInstanceAppliedHwmBytes ?? 0n,
    totalInstanceAccountedBytes: snapshot?.totalInstanceAccountedBytes ?? 0n,
    blockedRatioPpm: BigInt(Math.trunc(snapshot?.blockedRatioPpm ?? 0)),
    activeDirectionalQueueCount: snapshot?.activeDirectionalQueueCount ?? 0n,
    activeCompletionDirectionalQueueCount:
      snapshot?.activeCompletionDirectionalQueueCount ?? 0n,
    activeSendQueueCount: snapshot?.activeSendQueueCount ?? 0n,
    activeReceiveQueueCount: snapshot?.activeReceiveQueueCount ?? 0n,
    outstandingApplicationLeaseCount: snapshot?.outstandingApplicationLeaseCount ?? 0n,
    retiredQueueCount: snapshot?.retiredQueueCount ?? 0n,
    deferredOriginCreditBytes: snapshot?.deferredOriginCreditBytes ?? 0n
  });
}

function projectCoreProfile(value: AutoHwmProfileValue | undefined): ZLinkCoreHwmProfile {
  switch (value ?? AutoHwmProfile.Balanced) {
    case AutoHwmProfile.Compact: return ZLinkCoreHwmProfile.Compact;
    case AutoHwmProfile.LowLatency: return ZLinkCoreHwmProfile.LowLatency;
    case AutoHwmProfile.Throughput: return ZLinkCoreHwmProfile.Throughput;
    default: return ZLinkCoreHwmProfile.Balanced;
  }
}
