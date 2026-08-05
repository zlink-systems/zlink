using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkSessionActorBindingRegistry(ZLinkFrameworkRuntime runtime)
{
    private ZLinkSessionContext? _context;

    public IReadOnlyCollection<IZLinkSessionActor> BoundActors
    {
        get => _context is { } context
            ? runtime.SnapshotSessionActors(context)
            : [];
    }

    public ValueTask<IZLinkSessionActor> BindAsync(
        ZLinkSessionContext context,
        ActorRef actor,
        string bindingToken,
        ulong bindingGeneration,
        ulong authorityOwnerGeneration,
        string meshName,
        ulong targetNodeGeneration,
        ulong ownerLeaseGeneration,
        ulong sessionOwnerNodeGeneration,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var actorId = actor.ActorId;
        if (string.IsNullOrWhiteSpace(actorId)) throw new InvalidOperationException("Actor id must not be empty.");
        if (context.RoutingId is not { } sessionRid)
            throw new InvalidOperationException("Actor session binding requires a stream routing id.");

        var binding = new ZLinkSessionActorBinding(
            actorId,
            sessionRid,
            bindingToken);
        var route = ZLinkSessionBindingRoute.Create(
            actor,
            meshName,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        var actorRef = new ZLinkSessionActor(
            context,
            actorId,
            binding.SessionRid,
            binding.BindingToken);

        _context ??= context;
        if (!ReferenceEquals(_context, context))
            throw new InvalidOperationException(
                "A session Actor registry cannot be shared by different sessions.");
        _ = runtime.BindSessionActor(
            actorId,
            context,
            binding.BindingToken,
            actorRef,
            bindingGeneration,
            route,
            sessionOwnerNodeGeneration);

        return ValueTask.FromResult<IZLinkSessionActor>(actorRef);
    }

    public IZLinkSessionActor? FindActor(string actorId)
    {
        if (string.IsNullOrWhiteSpace(actorId)) return null;

        return _context is { } context
            ? runtime.FindSessionActor(context, actorId)
            : null;
    }

    public ValueTask ReleaseAsync(
        ZLinkSessionContext context,
        ZLinkSessionActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();

        runtime.UnbindSessionActor(actor.ActorId, context, actor.BindingToken);
        return ValueTask.CompletedTask;
    }

    public async ValueTask CleanupAsync(
        ZLinkSessionContext context,
        CancellationToken cancellationToken)
    {
        var actors = runtime.SnapshotSessionActors(context)
            .Cast<ZLinkSessionActor>()
            .ToArray();
        //  A session that ends with no bound actors and one that never reached
        //  cleanup look the same from the outside.

        // The transport disconnect owns one fixed binding snapshot. Notify
        // every exact binding concurrently, but bound each callback by the
        // runtime deadline so one Actor cannot hold session cleanup.
        await Task.WhenAll(actors
                .DistinctBy(actor => (
                    actor.ActorId,
                    actor.Ref.NodeRid,
                    actor.Ref.ObjectGeneration,
                    actor.BindingToken))
                .Select(NotifyBestEffortAsync))
            .ConfigureAwait(false);

        // Tombstones are always removed, including callback failure, timeout,
        // and a concurrent explicit notification of the same binding.
        foreach (var actor in actors)
        {
            runtime.UnbindSessionActor(
                actor.ActorId,
                context,
                actor.BindingToken);
        }

        return;

        async Task NotifyBestEffortAsync(ZLinkSessionActor actor)
        {
            try
            {
                await actor.NotifyDisconnectedAsync(cancellationToken)
                    .AsTask()
                    .WaitAsync(runtime.Registration.DefaultRequestTimeout, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception failure)
            {
                // Cleanup is all-settled. The exact binding token prevents a
                // stale or duplicate notification from affecting a replacement.
                // Settling quietly also hides a notification that never reached
                // the Actor's owner node, which leaves a binding whose session
                // is gone, so the drop is a traced one (spec 26 §2.1).
                if (runtime.Flow.Enabled(ZLinkMessageFlowOutcome.Dropped))
                    runtime.Flow.Trace(new ZLinkMessageFlowEvent(
                        ZLinkMessageFlowOutcome.Dropped,
                        ZLinkDispatchErrorSurface.StreamSession,
                        ZLinkDispatchMessageKind.Send,
                        ActorId: actor.ActorId,
                        ErrorReason: ZLinkDispatchErrorReason.ReplyPathMissing,
                        ErrorAction: ZLinkDispatchErrorAction.Drop,
                        ErrorType: failure.GetType().Name,
                        ErrorMessage: failure.Message));
            }
        }
    }

}

internal readonly record struct ZLinkSessionActorBinding(
    string ActorId,
    RoutingId SessionRid,
    string BindingToken);
