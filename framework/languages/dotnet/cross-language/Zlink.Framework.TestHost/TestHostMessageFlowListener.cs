using System.Diagnostics;

internal sealed class TestHostMessageFlowListener : IDisposable
{
    private const string ActivitySourceName = "Zlink.Framework";
    private readonly string _filePath;
    private readonly object _fileGate = new();
    private readonly ActivityListener _listener;

    public TestHostMessageFlowListener(string filePath)
    {
        _filePath = filePath;
        if (Path.GetDirectoryName(filePath) is { Length: > 0 } directory)
            Directory.CreateDirectory(directory);
        File.WriteAllText(filePath, string.Empty);
        _listener = new ActivityListener
        {
            ShouldListenTo = static source => source.Name == ActivitySourceName,
            Sample = static (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllData,
            ActivityStopped = Capture
        };
        ActivitySource.AddActivityListener(_listener);
    }

    public void Dispose() => _listener.Dispose();

    private void Capture(Activity activity)
    {
        if (!StringComparer.Ordinal.Equals(activity.OperationName, "zlink.message_flow"))
            return;

        var line =
            "zlink flow:"
            + $" label=dotnet-test-host"
            + $" phase={Tag(activity, "phase")}"
            + $" surface={Tag(activity, "surface")}"
            + $" kind={Tag(activity, "message_kind")}"
            + $" packet={Tag(activity, "packet_name")}"
            + $" flow={Tag(activity, "flow_id")}"
            + $" origin={Tag(activity, "flow_origin")}";
        lock (_fileGate) File.AppendAllText(_filePath, line + Environment.NewLine);
    }

    private static string? Tag(Activity activity, string name) =>
        activity.GetTagItem(name)?.ToString();
}
