namespace Zlink.Framework.Runtime.Configuration;

internal static partial class ZLinkFrameworkRegistrationValidator
{
    private static void ValidateLocations(ZLinkFrameworkRegistration registration)
    {
        var locations = registration.Locations;
        ValidateLocationPollingTimes(locations.Options);
        ValidateOwnerLeaseTimes(locations.Options);
        ValidateObjectRoutingTimes(locations.Options);
        ValidateRelocationTransferOptions(registration, locations.Options);
        if (locations.HasExplicitStore && locations.UseInMemoryStores)
        {
            throw new ZLinkConfigurationException(
                "AddLocationStore registers every store role at once and cannot be combined with "
                + "UseTestLocationStore.");
        }

    }

    private static void ValidateLocationPollingTimes(ZLinkLocationOptions options)
    {
        if (options.PollingInterval <= TimeSpan.Zero
            || options.StoreFailureGrace <= TimeSpan.Zero)
            throw new ZLinkConfigurationException(
                "PollingInterval and StoreFailureGrace must both be greater than zero.");
    }

    private static void ValidateOwnerLeaseTimes(ZLinkLocationOptions options)
    {
        if (options.OwnerLeaseRenewInterval <= TimeSpan.Zero
            || options.OwnerLeaseTtl <= TimeSpan.Zero
            || options.OwnerLeaseFencingMargin <= TimeSpan.Zero
            || options.OwnerLeaseRenewTimeout <= TimeSpan.Zero)
            throw new ZLinkConfigurationException(
                "Owner lease renewal, TTL, fencing margin, and renewal timeout "
                + "must all be greater than zero.");

        if (options.OwnerLeaseRenewInterval + options.OwnerLeaseRenewTimeout
            >= options.OwnerLeaseTtl - options.OwnerLeaseFencingMargin)
            throw new ZLinkConfigurationException(
                "OwnerLeaseRenewInterval + OwnerLeaseRenewTimeout must be "
                + "less than OwnerLeaseTtl - OwnerLeaseFencingMargin.");
    }

    private static void ValidateObjectRoutingTimes(ZLinkLocationOptions options)
    {
        if (options.RouteCacheMaxAge < TimeSpan.Zero
            || options.MessageFollowDuration < TimeSpan.Zero)
            throw new ZLinkConfigurationException(
                "RouteCacheMaxAge and MessageFollowDuration must be greater than or equal to zero.");

        if (options.RouteCacheMaxAge > TimeSpan.Zero
            && options.MessageFollowDuration > TimeSpan.Zero
            && options.RouteCacheMaxAge
               > options.MessageFollowDuration - TimeSpan.FromSeconds(5))
            throw new ZLinkConfigurationException(
                "RouteCacheMaxAge must be at least five seconds shorter than "
                + "MessageFollowDuration when both values are enabled.");

        var sealTimeout = options.SessionRelocationSealTimeout;
        if (sealTimeout <= TimeSpan.Zero
            || sealTimeout == Timeout.InfiniteTimeSpan
            || sealTimeout.Ticks % TimeSpan.TicksPerMillisecond != 0)
            throw new ZLinkConfigurationException(
                "SessionRelocationSealTimeout must be a positive duration "
                + "representable as exact whole milliseconds.");
    }

    private static void ValidateRelocationTransferOptions(
        ZLinkFrameworkRegistration registration,
        ZLinkLocationOptions options)
    {
        var frameLimit = registration.SpotNodes.Values
            .Select(static node => node.Router?.SocketConfig.MaxMessageSize)
            .Where(static size => size is not null)
            .DefaultIfEmpty(ZLinkSocketConfig.DefaultMaxMessageSize)
            .Min() ?? ZLinkSocketConfig.DefaultMaxMessageSize;
        if (options.RelocationPayloadChunkLimit <= 0
            || options.RelocationPayloadChunkLimit > frameLimit)
            throw new ZLinkConfigurationException(
                "RelocationPayloadChunkLimit must be greater than zero and "
                + "must not exceed the transport frame limit.");
        if (options.RelocationInFlightPayloadBudget < 0
            || options.RelocationNodeInFlightPayloadBudget < 0)
            throw new ZLinkConfigurationException(
                "RelocationInFlightPayloadBudget and "
                + "RelocationNodeInFlightPayloadBudget must be greater than "
                + "or equal to zero.");
        var cutoverWait = options.RelocationCutoverWaitTimeout;
        if (cutoverWait < TimeSpan.Zero
            || cutoverWait == Timeout.InfiniteTimeSpan)
            throw new ZLinkConfigurationException(
                "RelocationCutoverWaitTimeout must be a non-negative finite "
                + "duration.");
    }
}
