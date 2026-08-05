namespace Zlink.Framework.Runtime.Configuration;

internal sealed class ZLinkHandlerExposureCatalog
{
    private readonly IReadOnlyDictionary<string, HashSet<ZLinkHandlerGroupCatalogEntry>> _groups;
    private readonly IReadOnlyList<ZLinkHandlerExposure> _scannedChannels;

    private ZLinkHandlerExposureCatalog(
        IReadOnlyDictionary<string, HashSet<ZLinkHandlerGroupCatalogEntry>> groups,
        IReadOnlyList<ZLinkHandlerExposure> scannedChannels)
    {
        _groups = groups;
        _scannedChannels = scannedChannels;
    }

    public static ZLinkHandlerExposureCatalog Build(
        IReadOnlyList<ZLinkHandlerEndpointDescriptor> channelEndpoints)
    {
        var groups = new Dictionary<string, HashSet<ZLinkHandlerGroupCatalogEntry>>(StringComparer.Ordinal);
        var channels = channelEndpoints
            .Select(static endpoint => new ZLinkHandlerExposure(
                endpoint.Kind,
                endpoint.MessageName,
                endpoint.Groups))
            .ToArray();

        AddGroups(groups, ZLinkHandlerEndpointSurface.Channel, channels);
        return new ZLinkHandlerExposureCatalog(groups, channels);
    }

    public IReadOnlySet<ZLinkMessageKind> ValidateChannel(ZLinkChannelRegistration channel)
    {
        ValidateChannelMappedGroups(
            channel,
            channel.AutoConnectType == ZLinkLocationAutoConnectType.ClientServer
                ? MeshChannelKinds
                : FanoutKinds);

        ValidateExplicitChannelDuplicates(channel);
        var exposed = SelectMapped(_scannedChannels, channel.HandlerGroups);
        AddExplicitChannelHandlers(exposed, channel);
        ValidateConflicts(
            exposed,
            (kind, packetName) =>
                $"channel '{channel.ChannelName}' maps duplicate {kind} handler packet '{packetName}'.");
        return exposed.Select(static entry => entry.Kind).ToHashSet();
    }

    public void ValidateMeshNode(ZLinkSpotNodeRegistration node)
    {
        foreach (var membership in node.ChannelMemberships)
        {
            if (!membership.IsServer)
                continue;

            ValidateMappedGroups(
                $"channel '{node.SpotNodeName}:{membership.ChannelName}'",
                membership.HandlerGroups,
                MeshChannelKinds);

            ValidateExplicitDuplicates(
                membership.SendHandlers.Select(static handler =>
                    Explicit(handler, ZLinkMessageKind.Command)),
                packetName =>
                    $"Duplicate send handler '{node.SpotNodeName}:{membership.ChannelName}:{packetName}'.");
            ValidateExplicitDuplicates(
                membership.RequestHandlers.Select(static handler =>
                    Explicit(handler, ZLinkMessageKind.Request)),
                packetName =>
                    $"Duplicate request handler '{node.SpotNodeName}:{membership.ChannelName}:{packetName}'.");

            var exposed = SelectMapped(_scannedChannels, membership.HandlerGroups);
            foreach (var handler in membership.SendHandlers)
                exposed.Add(Explicit(handler, ZLinkMessageKind.Command));
            foreach (var handler in membership.RequestHandlers)
                exposed.Add(Explicit(handler, ZLinkMessageKind.Request));
            ValidateConflicts(
                exposed,
                (kind, packetName) =>
                    $"channel '{node.SpotNodeName}:{membership.ChannelName}' maps duplicate {kind} handler packet '{packetName}'.");
        }

        ValidateExplicitDuplicates(
            node.RouteSendHandlers.Select(static handler =>
                Explicit(handler, ZLinkMessageKind.Command)),
            packetName =>
                $"Duplicate routed send handler '{node.SpotNodeName}:{packetName}'.");
        ValidateExplicitDuplicates(
            node.RouteRequestHandlers.Select(static handler =>
                Explicit(handler, ZLinkMessageKind.Request)),
            packetName =>
                $"Duplicate routed request handler '{node.SpotNodeName}:{packetName}'.");
    }

    private void ValidateChannelMappedGroups(
        ZLinkChannelRegistration channel,
        IReadOnlySet<ZLinkMessageKind> allowedKinds)
        => ValidateMappedGroups(
            $"channel '{channel.ChannelName}'",
            channel.HandlerGroups,
            allowedKinds);

    private void ValidateMappedGroups(
        string owner,
        IReadOnlySet<string> handlerGroups,
        IReadOnlySet<ZLinkMessageKind> allowedKinds)
    {
        foreach (var group in handlerGroups)
        {
            if (!_groups.TryGetValue(group, out var entries))
                throw new ZLinkConfigurationException(
                    $"{owner} maps unknown handler group '{group}'.");

            foreach (var entry in entries)
                if (entry.Surface != ZLinkHandlerEndpointSurface.Channel || !allowedKinds.Contains(entry.Kind))
                    throw new ZLinkConfigurationException(
                        $"{owner} maps handler group '{group}' with incompatible handler kind '{entry.Kind}'.");
        }
    }

    private static void ValidateExplicitChannelDuplicates(ZLinkChannelRegistration channel)
    {
        ValidateExplicitDuplicates(
            channel.SendHandlers.Select(static handler => Explicit(handler, ZLinkMessageKind.Command)),
            packetName => $"Duplicate send handler '{channel.ChannelName}:{packetName}'.");
        ValidateExplicitDuplicates(
            channel.RequestHandlers.Select(static handler => Explicit(handler, ZLinkMessageKind.Request)),
            packetName => $"Duplicate request handler '{channel.ChannelName}:{packetName}'.");
        ValidateExplicitDuplicates(
            channel.PublishHandlers.Select(static handler => Explicit(handler, ZLinkMessageKind.Publish)),
            packetName => $"Duplicate publish handler '{channel.ChannelName}:{packetName}'.");
    }

    private static void ValidateExplicitDuplicates(
        IEnumerable<ZLinkHandlerExposure> handlers,
        Func<string, string> duplicateMessage)
    {
        var packets = new HashSet<string>(StringComparer.Ordinal);
        foreach (var handler in handlers)
            if (!packets.Add(handler.PacketName))
                throw new ZLinkConfigurationException(duplicateMessage(handler.PacketName));
    }

    private static List<ZLinkHandlerExposure> SelectMapped(
        IReadOnlyList<ZLinkHandlerExposure> scanned,
        IReadOnlySet<string> mappedGroups) => scanned
        .Where(endpoint => endpoint.Groups.Count > 0 && endpoint.Groups.Any(mappedGroups.Contains))
        .ToList();

    private static void AddExplicitChannelHandlers(
        ICollection<ZLinkHandlerExposure> exposed,
        ZLinkChannelRegistration channel)
    {
        foreach (var handler in channel.SendHandlers)
            exposed.Add(Explicit(handler, ZLinkMessageKind.Command));
        foreach (var handler in channel.RequestHandlers)
            exposed.Add(Explicit(handler, ZLinkMessageKind.Request));
        foreach (var handler in channel.PublishHandlers)
            exposed.Add(Explicit(handler, ZLinkMessageKind.Publish));
    }

    private static ZLinkHandlerExposure Explicit(
        ZLinkChannelHandlerRegistration handler,
        ZLinkMessageKind kind) => new(
        kind,
        ResolvePacketName(handler.MessageType, handler.PacketName),
        EmptyGroups);

    private static ZLinkHandlerExposure Explicit(
        ZLinkRouteHandlerRegistration handler,
        ZLinkMessageKind kind) => new(
        kind,
        ResolvePacketName(handler.MessageType, handler.PacketName),
        EmptyGroups);

    private static string ResolvePacketName(Type messageType, string? packetName) =>
        packetName ?? ZLinkMessageNameResolver.ResolveFromType(messageType);

    private static void ValidateConflicts(
        IEnumerable<ZLinkHandlerExposure> exposed,
        Func<ZLinkMessageKind, string, string> conflictMessage)
    {
        var handlers = new HashSet<(ZLinkMessageKind Kind, string PacketName)>();
        foreach (var handler in exposed)
        {
            var key = (handler.Kind, handler.PacketName);
            if (!handlers.Add(key))
                throw new ZLinkConfigurationException(conflictMessage(handler.Kind, handler.PacketName));
        }
    }

    private static void AddGroups(
        IDictionary<string, HashSet<ZLinkHandlerGroupCatalogEntry>> groups,
        ZLinkHandlerEndpointSurface surface,
        IEnumerable<ZLinkHandlerExposure> handlers)
    {
        foreach (var handler in handlers)
        foreach (var group in handler.Groups)
        {
            if (!groups.TryGetValue(group, out var entries))
            {
                entries = [];
                groups.Add(group, entries);
            }
            entries.Add(new ZLinkHandlerGroupCatalogEntry(surface, handler.Kind));
        }
    }

    private static readonly IReadOnlySet<string> EmptyGroups = new HashSet<string>(StringComparer.Ordinal);
    private static readonly IReadOnlySet<ZLinkMessageKind> FanoutKinds =
        new HashSet<ZLinkMessageKind> { ZLinkMessageKind.Publish };
    private static readonly IReadOnlySet<ZLinkMessageKind> MeshChannelKinds =
        new HashSet<ZLinkMessageKind> { ZLinkMessageKind.Command, ZLinkMessageKind.Request };

    private sealed record ZLinkHandlerExposure(
        ZLinkMessageKind Kind,
        string PacketName,
        IReadOnlySet<string> Groups);
}
