namespace Zlink.Framework.Runtime.Configuration;

internal static partial class ZLinkFrameworkRegistrationValidator
{
    public static void Validate(ZLinkFrameworkRegistration registration)
    {
        registration.FreezeScannedHandlerCatalog();
        var globalSpotFactories = new HashSet<Type>();
        var globalEntrySpots = new HashSet<Type>();
        var channelHandlerEndpoints = registration.ScannedHandlerCatalog.ChannelEndpoints;
        var handlerExposure = ZLinkHandlerExposureCatalog.Build(channelHandlerEndpoints);

        ValidateDispatchOptions(registration.DispatchOptions);

        ValidateLocations(registration);
        ValidateRelocationStoreRequirement(registration);

        foreach (var channel in registration.Channels.Values)
            ValidateChannel(
                channel,
                registration.Locations.Enabled,
                handlerExposure);

        foreach (var streamNode in registration.StreamNodes.Values) ValidateStreamNode(streamNode, registration);

        foreach (var spotNode in registration.SpotNodes.Values)
            ValidateSpotNode(
                spotNode,
                registration,
                globalSpotFactories,
                globalEntrySpots,
                handlerExposure);

        var clientServerChannels = registration.Channels.Values
            .Where(static channel =>
                channel.AutoConnectType == ZLinkLocationAutoConnectType.ClientServer)
            .Select(static channel => channel.ChannelName)
            .ToHashSet(StringComparer.Ordinal);
        foreach (var node in registration.SpotNodes.Values)
        foreach (var membership in node.ChannelMemberships)
            if (clientServerChannels.Contains(membership.ChannelName))
                throw new ZLinkConfigurationException(
                    $"ChannelName '{membership.ChannelName}' is registered on both RouteMesh and ClientServer physical paths.");

        registration.ActorCatalog.Build(registration.SpotNodes.Values);
    }

    internal static void ValidateInboundDispatch(ZLinkFrameworkRegistration registration)
    {
        var options = registration.InboundDispatchOptions;
        if (!Enum.IsDefined(options.ApplicationHwmProfile))
            throw new ZLinkConfigurationException(
                $"Unknown ApplicationHwmProfile value '{(int)options.ApplicationHwmProfile}'.");

        options.EffectiveApplicationHwmBytes = ZLinkApplicationHwmResolver.Resolve(options);
        if (options.EffectiveApplicationHwmBytes == 0)
            return;

        foreach (var (name, maximumMessageBytes) in EnumerateApplicationListenerLimits(registration))
            if (maximumMessageBytes <= 0)
                throw new ZLinkConfigurationException(
                    $"Application listener '{name}' must set a finite positive MaxMessageSize when ApplicationHwmBytes is Auto or positive.");
    }

    private static IEnumerable<(string Name, long MaximumMessageBytes)>
        EnumerateApplicationListenerLimits(ZLinkFrameworkRegistration registration)
    {
        foreach (var channel in registration.Channels.Values)
        {
            if (channel.Server is not null)
                yield return ($"ClientServer:{channel.ChannelName}",
                    channel.Server.SocketConfig.MaxMessageSize);
            if (channel.Subscriber is not null)
                yield return ($"Fanout:{channel.ChannelName}",
                    channel.Subscriber.SocketConfig.MaxMessageSize);
        }

        foreach (var node in registration.SpotNodes.Values)
            if (node.Router is not null)
                yield return ($"RouteMesh:{node.SpotNodeName}",
                    node.Router.SocketConfig.MaxMessageSize);

        foreach (var streamNode in registration.StreamNodes.Values)
            yield return ($"STREAM:{streamNode.StreamNodeName}",
                streamNode.SocketConfig.MaxMessageSize);
    }

    private static void ValidateRelocationStoreRequirement(
        ZLinkFrameworkRegistration registration)
    {
        var requiresRelocationStore = registration.SpotNodes.Values.Any(
            static node =>
                node.InstanceSpotFactories.Count > 0
                || node.SpotRelocations.Values.Any(
                    static relocation => relocation.PolicyKind != 0)
                || node.ActorRelocations.Values.Any(
                    static relocation => relocation.PolicyKind != 0));
        if (!requiresRelocationStore)
            return;

        if (registration.Locations.ResolveRelocationStore() is null)
            throw new ZLinkConfigurationException(
                "An Object Server with RecreateOnRelocation or PreserveStateWith, "
                + "or any Instance Spot factory, requires exactly one Relocation Store. "
                + "Register it via AddRelocationStore(...) before startup.");
    }

    private static void ValidateDispatchOptions(ZLinkDispatchOptionsModel options)
    {
        if (options.Unhandled.Send == ZLinkUnhandledDispatchAction.ReplyError)
            throw new ZLinkConfigurationException(
                "Unhandled send dispatch cannot use ReplyError because send has no reply path.");

        if (options.Unhandled.Publish == ZLinkUnhandledDispatchAction.ReplyError)
            throw new ZLinkConfigurationException(
                "Unhandled publish dispatch cannot use ReplyError because publish has no reply path.");

        if (double.IsNaN(options.Diagnostics.SampleRate)
            || options.Diagnostics.SampleRate < 0.0d
            || options.Diagnostics.SampleRate > 1.0d)
            throw new ZLinkConfigurationException(
                "Diagnostics SampleRate must be between 0.0 and 1.0.");
    }

    private static void ValidateStreamNode(
        ZLinkStreamNodeRegistration streamNode,
        ZLinkFrameworkRegistration registration)
    {
        if (streamNode.SocketConfig.MaxMessageSize < 0)
            throw new ZLinkConfigurationException(
                $"STREAM node '{streamNode.StreamNodeName}' MaxMessageSize must be zero or positive.");

        if (string.IsNullOrWhiteSpace(streamNode.BindEndpoint)
            && streamNode.ListenPort is null)
            throw new ZLinkConfigurationException(
                $"STREAM node '{streamNode.StreamNodeName}' must define a bind endpoint.");

        if (streamNode.HeaderSessionType is null)
            throw new ZLinkConfigurationException(
                $"STREAM node '{streamNode.StreamNodeName}' must register a header stream session.");

        if (!streamNode.ActorDispatchEnabled)
            return;
        if (!registration.Locations.Enabled)
            throw new ZLinkConfigurationException(
                $"STREAM node '{streamNode.StreamNodeName}' enables Actor dispatch but no Location Store is registered.");

        var objectMeshes = registration.SpotNodes.Values
            .Where(static node => node.ObjectRoleSelected)
            .ToArray();
        if (objectMeshes.Length == 0)
            throw new ZLinkConfigurationException(
                $"STREAM node '{streamNode.StreamNodeName}' requires at least one local Object Client or Server MeshNode.");
    }
}
