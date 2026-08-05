using System.Diagnostics;

namespace Zlink.Framework.E2E.Diagnostics;

internal sealed record E2eMessageFlow(
    string Phase,
    string Surface,
    string MessageKind,
    string? PacketName,
    string? ChannelName,
    string? Topic,
    string? Reason,
    string? Action,
    string? FlowId);

internal sealed class E2eMessageFlowListener : IDisposable
{
    internal const string ActivitySourceName = "Zlink.Framework";
    internal const string MeterName = "zlink.framework";

    private readonly string? _filePath;
    private readonly string? _label;
    private readonly Action<E2eMessageFlow>? _onFlow;
    private readonly object _fileGate = new();
    private readonly ActivityListener _listener;

    public E2eMessageFlowListener(
        string? filePath,
        string? label,
        Action<E2eMessageFlow>? onFlow = null)
    {
        _filePath = filePath;
        _label = label;
        _onFlow = onFlow;
        if (!string.IsNullOrWhiteSpace(_filePath))
        {
            if (Path.GetDirectoryName(_filePath) is { Length: > 0 } directory)
                Directory.CreateDirectory(directory);
            File.WriteAllText(_filePath, string.Empty);
        }

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

        var flow = new E2eMessageFlow(
            Tag(activity, "phase") ?? string.Empty,
            Tag(activity, "surface") ?? string.Empty,
            Tag(activity, "message_kind") ?? string.Empty,
            Tag(activity, "packet_name"),
            Tag(activity, "channel_name"),
            Tag(activity, "topic"),
            Tag(activity, "reason"),
            Tag(activity, "action"),
            Tag(activity, "flow_id"));

        if (!string.IsNullOrWhiteSpace(_filePath))
        {
            var line =
                "zlink flow:"
                + $" label={_label ?? string.Empty}"
                + $" phase={flow.Phase}"
                + $" surface={flow.Surface}"
                + $" kind={flow.MessageKind}"
                + $" packet={flow.PacketName ?? string.Empty}"
                + $" channel={flow.ChannelName ?? string.Empty}"
                + $" topic={flow.Topic ?? string.Empty}"
                + $" flow={flow.FlowId ?? string.Empty}"
                + $" reason={flow.Reason ?? string.Empty}"
                + $" action={flow.Action ?? string.Empty}";
            lock (_fileGate) File.AppendAllText(_filePath, line + Environment.NewLine);
        }

        if (_onFlow is null) return;
        try
        {
            _onFlow(flow);
        }
        catch
        {
            // Telemetry exporters must not change application message handling.
        }
    }

    private static string? Tag(Activity activity, string name) =>
        activity.GetTagItem(name)?.ToString();
}
