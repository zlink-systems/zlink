using System.Diagnostics;

internal sealed class TestHostMessageFlowListener : IDisposable
{
    private const string ActivitySourceName = "Zlink.Framework";
    private readonly string _filePath;
    private readonly TestHostEventSink _sink;
    private readonly TestHostEventSink? _ownedSink;
    private readonly ActivityListener _listener;

    // Unit tests and benches without a host sink get their own single writer.
    public TestHostMessageFlowListener(string filePath, Action<Activity>? observeStopped = null)
        : this(filePath, new TestHostEventSink(null), observeStopped, ownsSink: true) { }

    public TestHostMessageFlowListener(
        string filePath,
        TestHostEventSink sink,
        Action<Activity>? observeStopped = null
    )
        : this(filePath, sink, observeStopped, ownsSink: false) { }

    private TestHostMessageFlowListener(
        string filePath,
        TestHostEventSink sink,
        Action<Activity>? observeStopped,
        bool ownsSink
    )
    {
        _filePath = filePath;
        _sink = sink;
        _ownedSink = ownsSink ? sink : null;
        if (Path.GetDirectoryName(filePath) is { Length: > 0 } directory)
            Directory.CreateDirectory(directory);
        File.WriteAllText(filePath, string.Empty);
        _listener = new ActivityListener
        {
            ShouldListenTo = static source => source.Name == ActivitySourceName,
            Sample = static (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllData,
            ActivityStopped = activity =>
            {
                Capture(activity);
                observeStopped?.Invoke(activity);
            },
        };
        ActivitySource.AddActivityListener(_listener);
    }

    public void Dispose()
    {
        _listener.Dispose();
        _ownedSink?.Dispose();
    }

    public string[] ReadLines() => _sink.ReadLines(_filePath);

    public bool HasRelocationDetail(string actorId, string detail) =>
        ReadLines()
            .Any(line =>
            {
                var tagsOffset = line.IndexOf(" tags=", StringComparison.Ordinal);
                if (tagsOffset < 0)
                    return false;
                using var tags = System.Text.Json.JsonDocument.Parse(line[(tagsOffset + 6)..]);
                return tags.RootElement.TryGetProperty("actor_id", out var actor)
                    && actor.GetString() == actorId
                    && tags.RootElement.TryGetProperty("detail", out var stage)
                    && stage.GetString() == detail;
            });

    private void Capture(Activity activity)
    {
        var line =
            "zlink flow:"
            + $" label=dotnet-test-host"
            + $" phase={Tag(activity, "phase")}"
            + $" surface={Tag(activity, "surface")}"
            + $" kind={Tag(activity, "message_kind")}"
            + $" packet={Tag(activity, "packet_name")}"
            + $" flow={Tag(activity, "flow_id")}"
            + $" origin={Tag(activity, "flow_origin")}"
            + $" event={activity.OperationName}"
            + $" started={activity.StartTimeUtc:O}"
            + $" duration_ticks={activity.Duration.Ticks}"
            + FormatSourceRid(activity)
            + FormatTag(activity, "reason")
            + FormatTag(activity, "action")
            + FormatTag(activity, "error_message")
            + $" tags={System.Text.Json.JsonSerializer.Serialize(activity.TagObjects.ToDictionary(tag => tag.Key, tag => tag.Value))}";
        _sink.Append(_filePath, line);
    }

    // The Framework tags routing ids as hex; the stage script matches the text.
    private static string FormatSourceRid(Activity activity)
    {
        if (Tag(activity, "source_rid") is not { } hex)
            return string.Empty;
        try
        {
            var text = System.Text.Encoding.UTF8.GetString(Convert.FromHexString(hex));
            return text.All(char.IsAscii) && text.All(c => !char.IsControl(c))
                ? $" source_rid={text}"
                : $" source_rid={hex}";
        }
        catch (FormatException)
        {
            return $" source_rid={hex}";
        }
    }

    private static string FormatTag(Activity activity, string name) =>
        Tag(activity, name) is { } value ? $" {name}={value}" : string.Empty;

    private static string? Tag(Activity activity, string name) =>
        activity.GetTagItem(name)?.ToString();
}
