'use strict';
const { MeterProvider, MetricReader } = require('@opentelemetry/sdk-metrics');
// Collect when the coordinator requests a snapshot, outside the measured message
// path. Framework metric names, units and attributes pass through unchanged.
class SnapshotReader extends MetricReader {
  async onForceFlush() {}
  async onShutdown() {}
}
function publicMetrics() {
  const reader = new SnapshotReader();
  const provider = new MeterProvider({ readers: [reader] });
  return {
    provider,
    async snapshot() {
      const result = await reader.collect();
      if (result.errors.length) throw new AggregateError(result.errors, 'Public Framework metric collection failed.');
      return result.resourceMetrics.scopeMetrics.flatMap(scope => scope.metrics.map(metric => ({ name: metric.descriptor.name, unit: metric.descriptor.unit, description: metric.descriptor.description, aggregationTemporality: metric.aggregationTemporality, dataPointType: metric.dataPointType, dataPoints: metric.dataPoints, instrumentationScope: scope.scope })));
    },
    close: () => provider.shutdown()
  };
}
module.exports = { publicMetrics };
