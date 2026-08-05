using Microsoft.Extensions.DependencyInjection;

namespace Zlink.Framework.Runtime.Actors;

/// <summary>
/// Resolves and invokes the exact Actor relocation policy registered for one runtime node.
/// </summary>
internal static class ZLinkActorRelocationRegistry
{
    internal static ZLinkObjectRelocationRegistration Resolve(
        ZLinkFrameworkRegistration registration,
        string actorType,
        RoutingId nodeRid)
    {
        if (TryResolve(registration, actorType, nodeRid, out var relocation))
            return relocation;
        throw new ZLinkConfigurationException(
            $"Relocation policy for Actor type '{actorType}' is not registered.");
    }

    internal static bool TryResolve(
        ZLinkFrameworkRegistration registration,
        string actorType,
        RoutingId nodeRid,
        out ZLinkObjectRelocationRegistration relocation)
    {
        foreach (var node in registration.SpotNodes.Values)
            if (node.RoutingId == nodeRid
                && node.ActorRelocations.TryGetValue(actorType, out var registered))
            {
                relocation = registered;
                return true;
            }

        var matches = registration.SpotNodes.Values
            .Select(node => node.ActorRelocations.GetValueOrDefault(actorType))
            .Where(static relocation => relocation is not null)
            .Cast<ZLinkObjectRelocationRegistration>()
            .ToArray();
        if (matches.Length == 0)
        {
            relocation = null!;
            return false;
        }

        var first = matches[0];
        if (matches.Skip(1).Any(candidate =>
                candidate.InstanceType != first.InstanceType
                || candidate.PolicyKind != first.PolicyKind
                || candidate.AdapterType != first.AdapterType))
            throw new ZLinkConfigurationException(
                $"Relocation policy for Actor type '{actorType}' is ambiguous.");
        relocation = first;
        return true;
    }

    internal static async ValueTask<byte[]> CaptureAsync(
        IServiceProvider services,
        ZLinkObjectRelocationRegistration relocation,
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        return relocation.PolicyKind switch
        {
            0 => throw Disabled(relocation),
            1 => [],
            2 when relocation.AdapterInvoker is { } invoker =>
                await InvokeCaptureAsync(
                        services,
                        invoker,
                        actor,
                        cancellationToken)
                    .ConfigureAwait(false),
            _ => throw MissingAdapter(relocation)
        };
    }

    internal static async ValueTask RestoreAsync(
        IServiceProvider services,
        ZLinkObjectRelocationRegistration relocation,
        IZLinkActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        switch (relocation.PolicyKind)
        {
            case 0:
                throw Disabled(relocation);
            case 1 when payload.IsEmpty:
                return;
            case 1:
                throw new InvalidDataException(
                    $"Recreate relocation state for Actor type '{relocation.InstanceType}' must be empty.");
            case 2 when relocation.AdapterInvoker is { } invoker:
                await invoker.RestoreAsync(
                        services,
                        actor,
                        payload,
                        cancellationToken)
                    .ConfigureAwait(false);
                return;
            default:
                throw MissingAdapter(relocation);
        }
    }

    internal static ReadOnlyMemory<byte> ValidateIncomingPayload(
        ZLinkObjectRelocationRegistration relocation,
        string actorType,
        string contentType,
        ReadOnlyMemory<byte> applicationState)
    {
        var expectedContentType = relocation.PolicyKind switch
        {
            1 => ZLinkRemoteActorJoinPackets.RecreateRelocationContentType,
            2 => ZLinkRemoteActorJoinPackets.SnapshotRelocationContentType,
            0 => throw Disabled(relocation),
            _ => throw MissingAdapter(relocation)
        };
        if (!string.Equals(
                contentType,
                expectedContentType,
                StringComparison.Ordinal))
            throw new InvalidDataException(
                $"Actor type '{actorType}' relocation policy does not match the captured payload.");
        if (relocation.PolicyKind == 1 && !applicationState.IsEmpty)
            throw new InvalidDataException(
                $"Recreate relocation state for Actor type '{actorType}' must be empty.");
        return applicationState;
    }

    private static async ValueTask<byte[]> InvokeCaptureAsync(
        IServiceProvider services,
        IZLinkRelocationAdapterInvoker invoker,
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        await using var scope = services.CreateAsyncScope();
        return await invoker.CaptureAsync(
                scope.ServiceProvider,
                actor,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static ZLinkFrameworkException Disabled(
        ZLinkObjectRelocationRegistration relocation) =>
        new(
            ZLinkFrameworkErrorKind.Rejected,
            $"Relocation is disabled for Actor type '{relocation.InstanceType}'.");

    private static ZLinkConfigurationException MissingAdapter(
        ZLinkObjectRelocationRegistration relocation) =>
        new(
            $"Relocation adapter for Actor type '{relocation.InstanceType}' is not registered.");
}
