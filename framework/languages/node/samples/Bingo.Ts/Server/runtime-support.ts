import { ExportResultCode } from '@opentelemetry/core';
import {
  MeterProvider,
  PeriodicExportingMetricReader,
  type PushMetricExporter,
  type ResourceMetrics
} from '@opentelemetry/sdk-metrics';

type ShutdownOptions = {
  keepAlive?: boolean;
  signal?: AbortSignal;
};

const bingoMetricExporter: PushMetricExporter = {
  export(resourceMetrics: ResourceMetrics, result): void {
    for (const scope of resourceMetrics.scopeMetrics) {
      for (const metric of scope.metrics) {
        for (const point of metric.dataPoints) {
          const rawValue = point.value as number | { count?: number; sum?: number };
          const value = typeof rawValue === 'number' ? rawValue : rawValue.sum ?? rawValue.count ?? 0;
          console.log(
            `zlink metric name=${metric.descriptor.name} value=${value} attributes=${JSON.stringify(point.attributes)}`
          );
        }
      }
    }
    result({ code: ExportResultCode.SUCCESS });
  },
  forceFlush: async () => undefined,
  shutdown: async () => undefined
};

const bingoMeterProvider = new MeterProvider({
  readers: [new PeriodicExportingMetricReader({ exporter: bingoMetricExporter, exportIntervalMillis: 250 })]
});

function waitForShutdown(options: ShutdownOptions = {}): Promise<void> {
  return new Promise((resolve) => {
    const keepAlive = options.keepAlive === true
      ? setInterval(() => {}, 60000)
      : undefined;
    const stop = (): void => {
      if (keepAlive !== undefined) {
        clearInterval(keepAlive);
      }
      process.removeListener('SIGINT', stop);
      process.removeListener('SIGTERM', stop);
      options.signal?.removeEventListener('abort', stop);
      resolve(undefined);
    };
    if (options.signal?.aborted === true) stop();
    else options.signal?.addEventListener('abort', stop, { once: true });
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
  });
}

async function closeNestRuntime(container: { close(): Promise<void> }): Promise<void> {
  try {
    await container.close();
  } catch (error) {
    const candidate = error as { name?: string; code?: number };
    if (candidate.name === 'CloseError' && [0, 401, 403, 404].includes(candidate.code ?? -1)) {
      return;
    }
    throw error;
  } finally {
    await bingoMeterProvider.forceFlush();
    await bingoMeterProvider.shutdown();
  }
}

export { bingoMeterProvider, closeNestRuntime, waitForShutdown };
