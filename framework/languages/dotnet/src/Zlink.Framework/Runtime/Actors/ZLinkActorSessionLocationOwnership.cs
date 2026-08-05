namespace Zlink.Framework.Runtime.Actors;

internal sealed partial class ZLinkActorSessionManager
{
    public async ValueTask RollbackTransferredActorAsync(
        string actorId,
        CancellationToken cancellationToken = default,
        bool startTeardownReconciliation = true)
    {
        if (!_actorSessions.TryGet(actorId, out var state)) return;

        var actorRef = state.NativeActorRef;
        if (actorRef is not { } nativeActor)
            throw new InvalidOperationException(
                $"Actor '{actorId}' handoff rollback cannot complete without its native actor ref.");

        await state.ExecuteLockedAsync(
            state.BeginTeardown,
            CancellationToken.None).ConfigureAwait(false);

        try
        {
            await ExecuteActorTeardownAttemptAsync(state, nativeActor, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception cleanupFailure)
        {
            if (startTeardownReconciliation)
                StartActorTeardownReconciliation(state, nativeActor, "actor-handoff-rollback");
            throw new InvalidOperationException(
                $"Actor '{actorId}' handoff rollback is quarantined until cleanup can be reconciled.",
                cleanupFailure);
        }
    }

    public async ValueTask FinalizeMigratedSourceAsync(
        ZLinkActorRuntimeState state,
        ZLinkBackendActorRef sourceActor)
    {
        // The source stops owning disconnect cleanup, but its exact
        // bound-session fence remains available while Message Follow can
        // forward delayed frames to the target actor. Only a rebind or
        // disconnect invalidates that binding (spec 31 §6).
        if (state.TryGetBoundSession(out var boundSession))
            runtime.RetireMigratedActorSession(
                state.ActorId,
                boundSession.BindingToken);

        var terminal = state.BeginHandlerActivationCompletion(
                () =>
                {
                    state.RetireMigratedActorInstance(sourceActor);
                    return true;
                });
        if (terminal.RequiresDispatchRelease)
        {
            _ = ObserveDeferredMigratedSourceFinalizationAsync(
                state,
                terminal.Completion);
            return;
        }

        _ = await terminal.Completion.ConfigureAwait(false);
    }

    public async ValueTask PrepareForTransferredActivationAsync(
        ZLinkActorRuntimeState state,
        CancellationToken cancellationToken)
    {
        var retired = await state.ExecuteLockedAsync(
                () => state.RetiredLocalActorRef,
                cancellationToken)
            .ConfigureAwait(false);
        if (retired is { } retiredActor && getActorSpotNode() is { } node)
        {
            try
            {
                await node.DestroyActorAsync(
                        retiredActor,
                        runtime.Registration.DefaultRequestTimeout,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (ZlinkRequestException exception)
                when (exception.Result == ZlinkRequestException.ErrorCode.NotFound)
            {
            }

            await state.ExecuteLockedAsync(
                    () => state.ClearRetiredLocalActorRef(retiredActor),
                    CancellationToken.None)
                .ConfigureAwait(false);
        }

        await state.ExecuteLockedAsync(
                state.PrepareForTransferredActivation,
                cancellationToken)
            .ConfigureAwait(false);
    }

    /// <summary>
    /// Ownership-loss rule (location resolver store draft, section 9):
    /// another owner replaced this actor's location row, so the local
    /// instance must deactivate to keep single-activation. The context is
    /// invalidated first so in-flight callers fail fast. The native actor ref
    /// remains quarantined until native destruction reaches a terminal result;
    /// only then is ownership released and local state cleared.
    /// </summary>
    public async ValueTask DeactivateActorOnOwnershipLossAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        if (!_actorSessions.TryGet(actorId, out var state)) return;
        if (state.Handoff.IsSourceMigrationInProgress
            || (state.Actor is null && state.Handoff.RetainsSourceTombstone))
            return;

        var actorRef = await state.ExecuteLockedAsync<ZLinkBackendActorRef?>(
            () =>
            {
                if (state.Handoff.IsSourceMigrationInProgress
                    || (state.Actor is null && state.Handoff.RetainsSourceTombstone))
                    return null;
                var nativeRef = state.NativeActorRef;
                if (nativeRef is not null)
                    state.BeginTeardown();
                return nativeRef;
            },
            cancellationToken).ConfigureAwait(false);

        if (actorRef is not { } nativeActor) return;

        try
        {
            await ExecuteActorTeardownAttemptAsync(state, nativeActor, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"ownership-loss teardown retry for '{actorId}': {exception.Message}");
            StartActorTeardownReconciliation(state, nativeActor, "actor-ownership-loss");
        }
    }

    private async ValueTask ExecuteActorTeardownAttemptAsync(
        ZLinkActorRuntimeState state,
        ZLinkBackendActorRef nativeActor,
        CancellationToken cancellationToken)
    {
        var transaction = await state.ExecuteLockedAsync(
                state.BeginOrJoinTeardownAttempt,
                cancellationToken)
            .ConfigureAwait(false);
        if (!transaction.OwnsExecution)
        {
            var sharedFailure = await transaction.Completion.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            if (sharedFailure is not null)
                System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(sharedFailure).Throw();
            return;
        }

        var nativeDestroyed = transaction.NativeAlreadyDestroyed;
        var terminalStateCommitted = false;
        try
        {
            if (!nativeDestroyed)
            {
                var node = getActorSpotNode()
                           ?? throw new InvalidOperationException(
                               $"Actor '{state.ActorId}' teardown cannot confirm native destruction without a SpotNode.");
                try
                {
                    await node.DestroyActorAsync(
                            nativeActor,
                            runtime.Registration.DefaultRequestTimeout,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (ZlinkRequestException exception)
                    when (exception.Result == ZlinkRequestException.ErrorCode.NotFound)
                {
                }

                await state.ExecuteLockedAsync(
                        () => state.MarkNativeDestroyed(transaction),
                        CancellationToken.None)
                    .ConfigureAwait(false);
                nativeDestroyed = true;
            }

            if (LocationLifecycle is { } lifecycle)
                await lifecycle.ActorOwnership.ReleaseActorAsync(
                        state.ActorId,
                        cancellationToken)
                    .ConfigureAwait(false);

            var terminal = state.BeginHandlerActivationCompletion(
                    () =>
                    {
                        var result = state.CompleteTeardownAttempt(transaction);
                        terminalStateCommitted = true;
                        return result;
                    });
            if (terminal.RequiresDispatchRelease)
            {
                _ = CompleteDeferredActorTeardownAsync(
                    state,
                    nativeActor,
                    transaction,
                    nativeDestroyed,
                    terminal.Completion);
                return;
            }

            var boundSession = await terminal.Completion.ConfigureAwait(false);
            if (boundSession is { } session)
                runtime.RemoveActorSessionBinding(state.ActorId, session.BindingToken);
            _actorSessions.RemoveIfCurrent(state.ActorId, state);
        }
        catch (Exception failure)
        {
            if (terminalStateCommitted)
            {
                _actorSessions.RemoveIfCurrent(state.ActorId, state);
                throw;
            }
            await state.ExecuteLockedAsync(
                    () => state.FailTeardownAttempt(transaction, nativeDestroyed, failure),
                    CancellationToken.None)
                .ConfigureAwait(false);
            throw;
        }
    }

    private static async Task ObserveDeferredMigratedSourceFinalizationAsync(
        ZLinkActorRuntimeState state,
        Task<bool> completion)
    {
        try
        {
            _ = await completion.ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"deferred source finalization failed for actor '{state.ActorId}': {exception.Message}");
        }
    }

    private async Task CompleteDeferredActorTeardownAsync(
        ZLinkActorRuntimeState state,
        ZLinkBackendActorRef nativeActor,
        ZLinkActorTeardownOperation transaction,
        bool nativeDestroyed,
        Task<ZLinkActorBoundSession?> completion)
    {
        try
        {
            var boundSession = await completion.ConfigureAwait(false);
            if (boundSession is { } session)
                runtime.RemoveActorSessionBinding(
                    state.ActorId,
                    session.BindingToken);
            _actorSessions.RemoveIfCurrent(state.ActorId, state);
        }
        catch (Exception failure)
        {
            if (transaction.Completion.IsCompletedSuccessfully
                && transaction.Completion.Result is null)
            {
                _actorSessions.RemoveIfCurrent(state.ActorId, state);
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"deferred actor teardown disposal failed for '{state.ActorId}': {failure.Message}");
                return;
            }

            try
            {
                await state.ExecuteLockedAsync(
                        () => state.FailTeardownAttempt(
                            transaction,
                            nativeDestroyed,
                            failure),
                        CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (Exception reconciliationFailure)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"deferred actor teardown state reconciliation failed for '{state.ActorId}': {reconciliationFailure.Message}");
            }

            StartActorTeardownReconciliation(
                state,
                nativeActor,
                "actor-self-teardown");
        }
    }

    private void StartActorTeardownReconciliation(
        ZLinkActorRuntimeState state,
        ZLinkBackendActorRef nativeActor,
        string operationName)
    {
        if (!runtime.IsStarted) return;

        runtime.RunDetached(
            operationName,
            async cancellationToken =>
            {
                await ZLinkReconciliationRunner.RunAsync(
                        token => ExecuteActorTeardownAttemptAsync(state, nativeActor, token),
                        exception => ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"{operationName} retry for '{state.ActorId}': {exception.Message}"),
                        cancellationToken,
                        static exception => exception is OperationCanceledException)
                    .ConfigureAwait(false);
            });
    }

    internal async ValueTask CompensateUncommittedNativeActorAsync(
        IZLinkBackendSpotNode node,
        ZLinkBackendActorRef nativeActor,
        string operationName)
    {
        try
        {
            await DestroyUncommittedNativeActorAttemptAsync(node, nativeActor, CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (Exception cleanupFailure)
        {
            var scheduled = runtime.TryRunDetached(
                operationName,
                async cancellationToken =>
                {
                    await ZLinkReconciliationRunner.RunAsync(
                            token => DestroyUncommittedNativeActorAttemptAsync(node, nativeActor, token),
                            exception => ZLinkFrameworkDebugLog.SpotDiscovery(
                                $"{operationName} retry for '{nativeActor.ActorId}': {exception.Message}"),
                            cancellationToken,
                            static exception => exception is OperationCanceledException)
                        .ConfigureAwait(false);
                });
            throw new InvalidOperationException(
                scheduled
                    ? $"Actor '{nativeActor.ActorId}' admission cleanup is quarantined until native destruction can be reconciled."
                    : $"Actor '{nativeActor.ActorId}' admission cleanup failed after runtime reconciliation stopped.",
                cleanupFailure);
        }
    }

    private async ValueTask DestroyUncommittedNativeActorAttemptAsync(
        IZLinkBackendSpotNode node,
        ZLinkBackendActorRef nativeActor,
        CancellationToken cancellationToken)
    {
        try
        {
            await node.DestroyActorAsync(
                    nativeActor,
                    runtime.Registration.DefaultRequestTimeout,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZlinkRequestException exception)
            when (exception.Result == ZlinkRequestException.ErrorCode.NotFound)
        {
        }
    }

    /// <summary>
    /// The actor moved to another node whose runtime claimed the location
    /// itself (hosting handoff): this owner releases its row and stops
    /// tracking. A release racing the new owner's Takeover is ignored as
    /// stale by the store, which is the intended fencing outcome.
    /// </summary>
    public async ValueTask ReleaseActorLocationAfterMoveAsync(
        ZLinkActorRuntimeState state,
        CancellationToken cancellationToken = default)
    {
        if (LocationLifecycle is not { } lifecycle) return;

        await lifecycle.ActorOwnership.ReleaseActorAsync(
                state.ActorId,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask ReleaseActorLocationAfterMoveAsync(
        ZLinkActorRuntimeState state,
        ZLinkAuthoritySnapshot expectedSourceSnapshot,
        CancellationToken cancellationToken = default)
    {
        if (LocationLifecycle is not { } lifecycle) return;

        await lifecycle.ActorOwnership.ReleaseActorAfterMoveAsync(
                state.ActorId,
                expectedSourceSnapshot,
                cancellationToken)
            .ConfigureAwait(false);
    }

    /// <summary>
    /// The actor moved to another node's entry spot through the native
    /// join path, where no framework runtime claims the row on the target:
    /// this owner keeps the row and renews it with the new node rid so
    /// resolvers keep finding the actor.
    /// </summary>
    public async ValueTask RenewActorLocationAfterEntrySpotMoveAsync(
        ZLinkActorRuntimeState state,
        RoutingId targetNodeRid,
        CancellationToken cancellationToken = default)
    {
        if (LocationLifecycle is not { } lifecycle) return;

        await lifecycle.ActorOwnership.NotifyActorMovedToEntrySpotAsync(
                state.ActorId,
                targetNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
    }
}
