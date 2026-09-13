using System.Text.Json;

namespace ZLink.Framework.Perf;

public static class MetricCatalog
{
    private static readonly JsonDocument Catalog = JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "metric-catalog.json")));
    private static string[] Strings(string name) => Catalog.RootElement.GetProperty(name).EnumerateArray().Select(value => value.GetString()!).ToArray();
    public static readonly string[] LatencySuffixes = Strings("latencySuffixes");
    public static readonly string[] Outcomes = Strings("outcomes");
    private static readonly string[] Scalars = Strings("counts").Concat(Strings("rates")).Concat(Strings("unsupported"))
        .Concat(Strings("latencyPrefixes").SelectMany(prefix => LatencySuffixes.Select(suffix => prefix + "." + suffix))).ToArray();
    private static readonly string[] Histograms = Catalog.RootElement.GetProperty("histogramPrefixes").EnumerateObject().Select(property => property.Name).ToArray();
    public static void Null(Dictionary<string, object?> values, Dictionary<string, NullReason> reasons,
        string container, string key, string code, string reason)
    {
        values[key] = null;
        reasons["/" + container + "/" + key] = new(code, reason);
    }
    public static void BaselineNulls(Dictionary<string, object?> metrics, Dictionary<string, object?> histograms,
        Dictionary<string, NullReason> reasons)
    {
        foreach (var key in Scalars) Null(metrics, reasons, "metrics", key, "NOT_APPLICABLE", "This scenario does not own this observation.");
        foreach (var key in Histograms) Null(histograms, reasons, "histograms", key, "NOT_APPLICABLE", "This scenario does not measure this interval.");
        foreach (var suffix in new[] { "p50Ms", "p95Ms", "p99Ms" })
            Null(metrics, reasons, "metrics", "host.queueWaitLatency." + suffix,
                "PUBLIC_OBSERVATION_UNSUPPORTED", "Public status provides no exact pre-receive to handler queue-wait hook.");
    }
}
