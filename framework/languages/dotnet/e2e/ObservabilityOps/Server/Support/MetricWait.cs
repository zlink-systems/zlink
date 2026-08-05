using ObservabilityOps.Shared;

namespace ObservabilityOps.Server.Support;

public static class MetricWait
{
    public static bool Matches(MetricSample[] samples, MetricWaitReq request) =>
        samples.Any(sample => sample.Name == request.Name
                              && sample.Value >= request.MinimumValue
                              && (request.MaximumValue is null || sample.Value <= request.MaximumValue)
                              && (request.RequiredTags is null
                                  || request.RequiredTags.All(required =>
                                      sample.Tags.TryGetValue(required.Key, out var actual)
                                      && string.Equals(actual, required.Value, StringComparison.Ordinal))));
}
