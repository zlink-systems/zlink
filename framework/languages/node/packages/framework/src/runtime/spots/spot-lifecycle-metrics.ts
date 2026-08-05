import type { ZLinkRuntimeMetrics } from '../diagnostics';

export type ZLinkSpotMetricKind = 'entry' | 'user';

export class ZLinkSpotLifecycleMetrics {
  constructor(private readonly metrics?: ZLinkRuntimeMetrics) {}

  opened(kind: ZLinkSpotMetricKind): void {
    this.metrics?.change('zlink.spot.count', 1, { kind });
  }

  closed(kind: ZLinkSpotMetricKind): void {
    this.metrics?.change('zlink.spot.count', -1, { kind });
  }
}
