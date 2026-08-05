import type {
  ZLinkMeter,
  ZLinkMeterProvider,
  ZLinkMetricAttributes,
  ZLinkMetricHistogram,
  ZLinkMetricInstrument
} from '@zlink-systems/framework';

export interface MetricEvidence {
  readonly name: string;
  readonly kind: 'counter' | 'updown' | 'histogram';
  readonly value: number;
  readonly unit: string;
  readonly tags: ZLinkMetricAttributes;
}

export class MetricEvidenceCollector {
  private readonly values = new Map<string, MetricEvidence>();

  readonly provider: ZLinkMeterProvider = {
    getMeter: () => this.meter
  };

  snapshot(): readonly MetricEvidence[] {
    return [...this.values.values()].map((value) => ({ ...value, tags: { ...value.tags } }));
  }

  private readonly meter: ZLinkMeter = {
    createCounter: (name, options) => this.instrument(name, 'counter', options?.unit),
    createUpDownCounter: (name, options) => this.instrument(name, 'updown', options?.unit),
    createHistogram: (name, options) => this.histogram(name, options?.unit)
  };

  private instrument(
    name: string,
    kind: 'counter' | 'updown',
    unit = '{event}'
  ): ZLinkMetricInstrument {
    return {
      add: (value, attributes = {}) => this.record(name, kind, value, unit, attributes, true)
    };
  }

  private histogram(name: string, unit = 's'): ZLinkMetricHistogram {
    return {
      record: (value, attributes = {}) => this.record(name, 'histogram', value, unit, attributes, false)
    };
  }

  private record(
    name: string,
    kind: MetricEvidence['kind'],
    value: number,
    unit: string,
    tags: ZLinkMetricAttributes,
    accumulate: boolean
  ): void {
    const normalizedTags = Object.fromEntries(Object.entries(tags).sort(([left], [right]) => left.localeCompare(right)));
    const key = `${name}|${kind}|${JSON.stringify(normalizedTags)}`;
    const previous = this.values.get(key)?.value ?? 0;
    this.values.set(key, {
      name,
      kind,
      value: accumulate ? previous + value : value,
      unit,
      tags: normalizedTags
    });
  }
}
