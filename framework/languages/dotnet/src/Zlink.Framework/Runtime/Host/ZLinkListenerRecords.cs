using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Host;

/// <summary>
/// Bound listener records of one runtime generation (spec 04 §3.1). A listener writes its record
/// when it finishes binding. The listener status query reads only this object.
/// </summary>
internal sealed class ZLinkListenerRecords
{
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<(ZLinkListenerKind Kind, string Name), string> _endpoints = [];

    internal ValueTask RecordAsync(ZLinkListenerKind kind, string name, string endpoint) =>
        _lane.RunAsync(() =>
        {
            _endpoints[(kind, name)] = endpoint;
        });

    internal ValueTask<string?> ReadAsync(ZLinkListenerKind kind, string name) =>
        _lane.RunAsync(() => _endpoints.GetValueOrDefault((kind, name)));

    internal ValueTask ClearAsync() => _lane.RunAsync(_endpoints.Clear);
}
