import { METRIC_NAMES } from '../diagnostics/runtime-metrics';
import type { ZLinkRuntimeMetrics } from '../diagnostics';

export type ZLinkSpotMetricKind = 'entry' | 'user';

export class ZLinkSpotLifecycleMetrics {
  constructor(private readonly metrics?: ZLinkRuntimeMetrics) {}

  opened(kind: ZLinkSpotMetricKind): void {
    this.metrics?.change(METRIC_NAMES.SpotCount, 1, { kind });
  }

  closed(kind: ZLinkSpotMetricKind): void {
    this.metrics?.change(METRIC_NAMES.SpotCount, -1, { kind });
  }
}
