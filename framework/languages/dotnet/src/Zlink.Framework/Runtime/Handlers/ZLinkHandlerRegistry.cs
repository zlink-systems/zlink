using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Handlers;

internal sealed class ZLinkHandlerRegistry
{
    private readonly IReadOnlyDictionary<string, IReadOnlyList<ZLinkHandlerEndpointDescriptor>> _commands;
    private readonly IReadOnlyDictionary<string, IReadOnlyList<ZLinkHandlerEndpointDescriptor>> _publishes;

    private readonly ConcurrentDictionary<ZLinkHandlerSelectionKey, IReadOnlyList<ZLinkHandlerEndpointDescriptor>>
        _publishSelections = new();

    private readonly IReadOnlyDictionary<string, IReadOnlyList<ZLinkHandlerEndpointDescriptor>> _requests;

    private readonly ConcurrentDictionary<ZLinkHandlerSelectionKey, ZLinkHandlerEndpointDescriptor> _singleSelections =
        new();

    public ZLinkHandlerRegistry(IEnumerable<ZLinkHandlerEndpointDescriptor> endpoints)
    {
        var requests = new Dictionary<string, List<ZLinkHandlerEndpointDescriptor>>(StringComparer.Ordinal);
        var commands = new Dictionary<string, List<ZLinkHandlerEndpointDescriptor>>(StringComparer.Ordinal);
        var publishes = new Dictionary<string, List<ZLinkHandlerEndpointDescriptor>>(StringComparer.Ordinal);

        foreach (var endpoint in endpoints)
            switch (endpoint.Kind)
            {
                case ZLinkMessageKind.Request:
                    AddEndpoint(requests, endpoint);
                    break;
                case ZLinkMessageKind.Command:
                    AddEndpoint(commands, endpoint);
                    break;
                case ZLinkMessageKind.Publish:
                    AddEndpoint(publishes, endpoint);
                    break;
            }

        _requests = Freeze(requests);
        _commands = Freeze(commands);
        _publishes = Freeze(publishes);
    }

    public ZLinkHandlerEndpointDescriptor GetRequest(
        string channelName,
        IReadOnlySet<string> mappedGroups,
        string messageName)
    {
        return GetSingle(
            ZLinkMessageKind.Request,
            _requests,
            channelName,
            mappedGroups,
            messageName,
            "request",
            "No request handler is registered");
    }

    public bool TryGetRequest(
        string channelName,
        IReadOnlySet<string> mappedGroups,
        string messageName,
        out ZLinkHandlerEndpointDescriptor? descriptor)
    {
        return TryGetSingle(
            ZLinkMessageKind.Request,
            _requests,
            channelName,
            mappedGroups,
            messageName,
            "request",
            out descriptor);
    }

    public ZLinkHandlerEndpointDescriptor GetCommand(
        string channelName,
        IReadOnlySet<string> mappedGroups,
        string messageName)
    {
        return GetSingle(
            ZLinkMessageKind.Command,
            _commands,
            channelName,
            mappedGroups,
            messageName,
            "send",
            "No send handler is registered");
    }

    public bool TryGetCommand(
        string channelName,
        IReadOnlySet<string> mappedGroups,
        string messageName,
        out ZLinkHandlerEndpointDescriptor? descriptor)
    {
        return TryGetSingle(
            ZLinkMessageKind.Command,
            _commands,
            channelName,
            mappedGroups,
            messageName,
            "send",
            out descriptor);
    }

    public IReadOnlyList<ZLinkHandlerEndpointDescriptor> GetPublishes(
        string channelName,
        IReadOnlySet<string> mappedGroups,
        string messageName)
    {
        var key = new ZLinkHandlerSelectionKey(ZLinkMessageKind.Publish, channelName, messageName);
        if (_publishSelections.TryGetValue(key, out var cached)) return cached;

        var selected = _publishes.TryGetValue(messageName, out var endpoints)
            ? FilterEndpoints(channelName, mappedGroups, endpoints)
            : Array.Empty<ZLinkHandlerEndpointDescriptor>();
        return _publishSelections.GetOrAdd(key, selected);
    }

    private static void AddEndpoint(
        Dictionary<string, List<ZLinkHandlerEndpointDescriptor>> targets,
        ZLinkHandlerEndpointDescriptor endpoint)
    {
        if (!targets.TryGetValue(endpoint.MessageName, out var list))
        {
            list = [];
            targets.Add(endpoint.MessageName, list);
        }

        list.Add(endpoint);
    }

    private static IReadOnlyDictionary<string, IReadOnlyList<ZLinkHandlerEndpointDescriptor>> Freeze(
        Dictionary<string, List<ZLinkHandlerEndpointDescriptor>> source)
    {
        return source.ToDictionary(
            static entry => entry.Key,
            static entry => (IReadOnlyList<ZLinkHandlerEndpointDescriptor>)entry.Value.ToArray(),
            StringComparer.Ordinal);
    }

    private ZLinkHandlerEndpointDescriptor GetSingle(
        ZLinkMessageKind kind,
        IReadOnlyDictionary<string, IReadOnlyList<ZLinkHandlerEndpointDescriptor>> registry,
        string channelName,
        IReadOnlySet<string> mappedGroups,
        string messageName,
        string kindName,
        string missingMessage)
    {
        var key = new ZLinkHandlerSelectionKey(kind, channelName, messageName);
        if (_singleSelections.TryGetValue(key, out var cached)) return cached;

        var selected = registry.TryGetValue(messageName, out var endpoints)
            ? SelectEndpoint(channelName, mappedGroups, messageName, endpoints, kindName)
            : throw new InvalidOperationException($"{missingMessage} for '{messageName}'.");
        return _singleSelections.GetOrAdd(key, selected);
    }

    private bool TryGetSingle(
        ZLinkMessageKind kind,
        IReadOnlyDictionary<string, IReadOnlyList<ZLinkHandlerEndpointDescriptor>> registry,
        string channelName,
        IReadOnlySet<string> mappedGroups,
        string messageName,
        string kindName,
        out ZLinkHandlerEndpointDescriptor? descriptor)
    {
        var key = new ZLinkHandlerSelectionKey(kind, channelName, messageName);
        if (_singleSelections.TryGetValue(key, out var cached))
        {
            descriptor = cached;
            return true;
        }

        if (!registry.TryGetValue(messageName, out var endpoints))
        {
            descriptor = null;
            return false;
        }

        var matches = FilterEndpoints(channelName, mappedGroups, endpoints);
        switch (matches.Count)
        {
            case 0:
                descriptor = null;
                return false;
            case 1:
                descriptor = _singleSelections.GetOrAdd(key, matches[0]);
                return true;
            default:
                throw new ZLinkConfigurationException(
                    $"Duplicate {kindName} handler packet '{messageName}' is mapped to channel '{channelName}'.");
        }
    }

    private static ZLinkHandlerEndpointDescriptor SelectEndpoint(
        string channelName,
        IReadOnlySet<string> mappedGroups,
        string messageName,
        IReadOnlyList<ZLinkHandlerEndpointDescriptor> endpoints,
        string kind)
    {
        var matches = FilterEndpoints(channelName, mappedGroups, endpoints);
        return matches.Count switch
        {
            1 => matches[0],
            0 => throw new InvalidOperationException(
                $"No {kind} handler is mapped for '{channelName}:{messageName}'."),
            _ => throw new ZLinkConfigurationException(
                $"Duplicate {kind} handler packet '{messageName}' is mapped to channel '{channelName}'.")
        };
    }

    private static IReadOnlyList<ZLinkHandlerEndpointDescriptor> FilterEndpoints(
        string channelName,
        IReadOnlySet<string> mappedGroups,
        IReadOnlyList<ZLinkHandlerEndpointDescriptor> endpoints)
    {
        List<ZLinkHandlerEndpointDescriptor>? matches = null;
        foreach (var endpoint in endpoints)
        {
            if (!IsMappedToChannel(endpoint, channelName, mappedGroups)) continue;

            matches ??= [];
            matches.Add(endpoint);
        }

        return matches is null
            ? Array.Empty<ZLinkHandlerEndpointDescriptor>()
            : matches.ToArray();
    }

    private static bool IsMappedToChannel(
        ZLinkHandlerEndpointDescriptor endpoint,
        string channelName,
        IReadOnlySet<string> mappedGroups)
    {
        if (endpoint.ExplicitChannelName is not null)
            return string.Equals(endpoint.ExplicitChannelName, channelName, StringComparison.Ordinal);

        return mappedGroups.Count > 0
               && endpoint.Groups.Count > 0
               && endpoint.Groups.Any(mappedGroups.Contains);
    }
}