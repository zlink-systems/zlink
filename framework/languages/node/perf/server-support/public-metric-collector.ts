import { DataPointType, MeterProvider, MetricReader } from '@opentelemetry/sdk-metrics';
import type { ZLinkMeterProvider } from '@zlink-systems/framework';

// Standard OpenTelemetry provider observation, restricted to the documented host capacity instruments
// (zlink.host.core_hwm.*, zlink.host.application_job_queue.*). The Framework registers them on the meter provider
// this host hands it; a reader collects them on demand. Nothing here synthesizes provider metrics.
class OnDemandReader extends MetricReader {
  protected async onShutdown(): Promise<void> {}
  protected async onForceFlush(): Promise<void> {}
}

export interface PublicMetricObservation {
  name: string;
  kind: 'counter' | 'observable';
  unit: string;
  labels: Record<string, unknown>;
  value: string | number;
  meter: string;
}

export class PublicMetricCollector {
  private readonly reader = new OnDemandReader();
  private readonly provider = new MeterProvider({ readers: [this.reader] });

  // The provider the Framework host is configured with (options({ metrics: { meterProvider } })).
  get meterProvider(): ZLinkMeterProvider {
    return this.provider as unknown as ZLinkMeterProvider;
  }

  async snapshot(): Promise<PublicMetricObservation[]> {
    const { resourceMetrics } = await this.reader.collect();
    const observations: PublicMetricObservation[] = [];
    for (const scope of resourceMetrics.scopeMetrics) {
      for (const metric of scope.metrics) {
        const name = metric.descriptor.name;
        if (!name.startsWith('zlink.host.core_hwm.') && !name.startsWith('zlink.host.application_job_queue.')) continue;
        for (const point of metric.dataPoints) {
          const value = point.value as number;
          if (typeof value !== 'number' || !Number.isFinite(value)) throw new Error('Provider returned a non-numeric or non-finite public metric.');
          observations.push({
            name, kind: metric.dataPointType === DataPointType.SUM ? 'counter' : 'observable', unit: metric.descriptor.unit,
            labels: { ...point.attributes }, value: Number.isInteger(value) ? String(value) : value, meter: scope.scope.name
          });
        }
      }
    }
    if (observations.length === 0) throw new Error('No public host capacity metrics were collected.');
    return observations;
  }
}
