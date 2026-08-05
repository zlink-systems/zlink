export const ZLinkMeters = Object.freeze({ Framework: 'zlink.framework' } as const);

export interface ZLinkMetricAttributes {
  readonly [name: string]: string | number | boolean;
}

export interface ZLinkMetricInstrument {
  add(value: number, attributes?: ZLinkMetricAttributes): void;
}

export interface ZLinkMetricHistogram {
  record(value: number, attributes?: ZLinkMetricAttributes): void;
}

export interface ZLinkMeter {
  createCounter(name: string, options?: { readonly unit?: string }): ZLinkMetricInstrument;
  createUpDownCounter(name: string, options?: { readonly unit?: string }): ZLinkMetricInstrument;
  createHistogram(name: string, options?: { readonly unit?: string }): ZLinkMetricHistogram;
}

export interface ZLinkMeterProvider {
  getMeter(name: string): ZLinkMeter;
}

export interface ZLinkMetricsOptions {
  readonly meterProvider?: ZLinkMeterProvider;
}
