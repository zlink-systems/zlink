export enum ZLinkApplicationHwmProfile {
  Compact = 'compact',
  LowLatency = 'low_latency',
  Balanced = 'balanced',
  Throughput = 'throughput'
}

export interface ZLinkInboundDispatchOptions {
  applicationHwmBytes(value: bigint | undefined): this;
  applicationHwmProfile(value: ZLinkApplicationHwmProfile): this;
  processMemoryLimitBytes(value: bigint | undefined): this;
}

export interface ZLinkInboundDispatchOptionValues {
  applicationHwmBytes?: bigint;
  applicationHwmProfile: ZLinkApplicationHwmProfile;
  processMemoryLimitBytes?: bigint;
}
