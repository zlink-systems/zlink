using Microsoft.Extensions.DependencyInjection;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorCreationCoordinator(
    ZLinkFrameworkRuntime runtime,
    IServiceProvider services,
    Func<IZLinkBackendSpotNode?> getActorSpotNode,
    IZLinkActorLocationLifecycle? lifecycle,
    Func<ZLinkActorRuntimeState, ZLinkActorContext> ensureActorContext,
    Func<IZLinkActor, ZLinkActorRuntimeState, ZLinkActorContext> bindActorContext,
    Func<ZLinkActorRuntimeState, ZLinkBackendActorRef, CancellationToken, ValueTask> teardownActor,
    Func<string, ZLinkActivationConcurrencyAdmission?>? getActivationAdmission = null)
{
    private IZLinkActorLocationLifecycle? Lifecycle { get; } = lifecycle;

    public async ValueTask<CreateActorResult> CreateAndBindActorAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        bool failIfExists,
        ZLinkActorClaimMode claimMode,
        CancellationToken cancellationToken)
    {
        var factoryType = ResolveActorFactory(actorType);

        var creation = await state.GetOrStartActorCreationAsync(
                actorType,
                failIfExists,
                () => CreateActorCoreAsync(
                    state,
                    actorId,
                    actorType,
                    factoryType,
                    createRequest,
                    claimMode,
                    CancellationToken.None).AsTask(),
                cancellationToken)
            .ConfigureAwait(false);

        // A waiter's cancellation never owns shared creation cleanup. The
        // state observes the shared task itself and clears only its failure.
        var actor = await creation.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        return new CreateActorResult(
            actor,
            creation.Created,
            creation.Created ? createRequest : ZLinkMessage.Empty);
    }

    public async ValueTask<CreateActorResult> PrepareReservedActorAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        var factoryType = ResolveActorFactory(actorType);
        state.BeginReservedCreation();
        try
        {
            var creation = await state.GetOrStartActorCreationAsync(
                    actorType,
                    true,
                    () => ActivateActorCoreAsync(
                        state,
                        actorId,
                        actorType,
                        factoryType,
                        createRequest,
                        CancellationToken.None,
                        objectGeneration,
                        authorityOwnerGeneration).AsTask(),
                    cancellationToken)
                .ConfigureAwait(false);
            var actor = await creation.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            return new CreateActorResult(
                actor,
                creation.Created,
                creation.Created ? createRequest : ZLinkMessage.Empty);
        }
        catch
        {
            state.PublishReservedCreation();
            throw;
        }
    }

    public async ValueTask<CreateActorResult> RelocateAndBindActorAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        string actorType,
        ZLinkObjectRelocationRegistration relocation,
        ReadOnlyMemory<byte> relocationState,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ZLinkActorClaimMode claimMode,
        bool publishActorRef,
        CancellationToken cancellationToken)
    {
        var creation = await state.GetOrStartActorCreationAsync(
                actorType,
                false,
                () => CreateRelocatedActorCoreAsync(
                    state,
                    actorId,
                    actorType,
                    relocation,
                    relocationState,
                    objectGeneration,
                    authorityOwnerGeneration,
                    claimMode,
                    CancellationToken.None,
                    publishActorRef).AsTask(),
                cancellationToken)
            .ConfigureAwait(false);

        // A relocation creation belongs to the handoff transaction once it
        // starts. Observe it to a terminal result so cancellation cannot
        // detach a late actor/claim from rollback ownership.
        var actor = await creation.Task.ConfigureAwait(false);
        return new CreateActorResult(actor, creation.Created, ZLinkMessage.Empty);
    }

    private Type ResolveActorFactory(string actorType)
    {
        return runtime.Registration.ActorCatalog.ResolveFactory(actorType);
    }

    private async ValueTask<IZLinkActor> CreateRelocatedActorCoreAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        string actorType,
        ZLinkObjectRelocationRegistration relocation,
        ReadOnlyMemory<byte> relocationState,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ZLinkActorClaimMode claimMode,
        CancellationToken cancellationToken,
        bool publishActorRef)
    {
        var factoryType = ResolveActorFactory(actorType);
        if (Lifecycle is not { } lifecycle)
            return await ActivateRelocatedActorCoreAsync(
                    state,
                    actorId,
                    actorType,
                    factoryType,
                    relocation,
                    relocationState,
                    objectGeneration,
                    authorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false);

        var outcome = await lifecycle.ExecuteActorClaimThenActivateAsync(
                Host.ZLinkActorDrainCoordinator.ResolveMeshName(runtime.Registration, actorType)
                    ?? string.Empty,
                actorType,
                actorId,
                getActorSpotNode()?.RoutingId ?? default,
                deactivate: _ => runtime.DeactivateActorOnOwnershipLossAsync(actorId),
                activate: ct => ActivateRelocatedActorCoreAsync(
                    state,
                    actorId,
                    actorType,
                    factoryType,
                    relocation,
                    relocationState,
                    objectGeneration,
                    authorityOwnerGeneration,
                    ct),
                cancellationToken,
                claimMode)
            .ConfigureAwait(false);
        if (outcome.Activated is not { } actor)
        {
            var location = outcome.ExistingLocation;
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                location is null
                    ? $"Actor '{actorId}' relocation claim was rejected and no live location row was found."
                    : $"Actor '{actorId}' is already active on node '{location.OwnerNodeRid}' (relocation claim conflict).");
        }

        if (publishActorRef && state.NativeActorRef is { } nativeRef)
            await PublishActorRefOrCompensateAsync(
                    state,
                    actorId,
                    nativeRef,
                    lifecycle,
                    cancellationToken)
                .ConfigureAwait(false);
        return actor;
    }

    private async ValueTask<IZLinkActor> ActivateRelocatedActorCoreAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        string actorType,
        Type factoryType,
        ZLinkObjectRelocationRegistration relocation,
        ReadOnlyMemory<byte> relocationState,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        var activationAdmission = getActivationAdmission?.Invoke(actorType);
        activationAdmission?.Acquire($"ACTOR '{actorId}'");
        try
        {
            await using var scope = services.CreateAsyncScope();
            EnsureNativeActorRef(
                state,
                actorId,
                ZLinkMessage.Empty,
                objectGeneration,
                authorityOwnerGeneration);
            var context = ensureActorContext(state);
            try
            {
                var factory = (IZLinkActorFactory)scope.ServiceProvider.GetRequiredService(factoryType);
                var actor = await factory.CreateAsync(context, cancellationToken)
                    .ConfigureAwait(false);
                if (actor is null)
                    throw new InvalidOperationException($"Actor factory '{factoryType}' returned null.");
                if (!ReferenceEquals(actor.Context, context))
                    throw new InvalidOperationException(
                        $"Actor factory '{factoryType}' must return an Actor that exposes the provided context.");

                await ZLinkActorRelocationRegistry.RestoreAsync(
                        scope.ServiceProvider,
                        relocation,
                        actor,
                        relocationState,
                        cancellationToken)
                    .ConfigureAwait(false);
                bindActorContext(actor, state);
                return actor;
            }
            catch (Exception activationFailure)
            {
                await DestroyStagedNativeActorAsync(state, activationFailure).ConfigureAwait(false);
                throw;
            }
        }
        finally
        {
            activationAdmission?.Release();
        }
    }

    private async ValueTask<IZLinkActor> CreateActorCoreAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        string actorType,
        Type factoryType,
        ZLinkMessage createRequest,
        ZLinkActorClaimMode claimMode,
        CancellationToken cancellationToken,
        bool publishActorRef = true,
        ulong? reservedGeneration = null,
        ulong? reservedAuthorityOwnerGeneration = null)
    {
        if (Lifecycle is not { } lifecycle)
            return await ActivateActorCoreAsync(
                    state,
                    actorId,
                    actorType,
                    factoryType,
                    createRequest,
                    cancellationToken,
                    reservedGeneration,
                    reservedAuthorityOwnerGeneration)
                .ConfigureAwait(false);

        // Claim-then-activate (location resolver store draft, section 17):
        // the actor location claim must succeed before any instance exists.
        // A losing claimer backs off without activating.
        var outcome = await lifecycle.ExecuteActorClaimThenActivateAsync(
                Host.ZLinkActorDrainCoordinator.ResolveMeshName(runtime.Registration, actorType)
                    ?? string.Empty,
                actorType,
                actorId,
                getActorSpotNode()?.RoutingId ?? default,
                deactivate: _ => runtime.DeactivateActorOnOwnershipLossAsync(actorId),
                activate: ct => ActivateActorCoreAsync(
                    state,
                    actorId,
                    actorType,
                    factoryType,
                    createRequest,
                    ct,
                    reservedGeneration,
                    reservedAuthorityOwnerGeneration),
                cancellationToken,
                claimMode)
            .ConfigureAwait(false);
        if (outcome.Activated is not { } actor)
        {
            var location = outcome.ExistingLocation;
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                location is null
                    ? $"Actor '{actorId}' location claim was rejected and no live location row was found."
                    : $"Actor '{actorId}' is already active on node '{location.OwnerNodeRid}' (location claim conflict).");
        }

        if (publishActorRef && state.NativeActorRef is { } nativeRef)
            await PublishActorRefOrCompensateAsync(
                    state,
                    actorId,
                    nativeRef,
                    lifecycle,
                    cancellationToken)
                .ConfigureAwait(false);

        return actor;
    }

    private async ValueTask<IZLinkActor> ActivateActorCoreAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        string actorType,
        Type factoryType,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken,
        ulong? reservedGeneration = null,
        ulong? reservedAuthorityOwnerGeneration = null)
    {
        var activationAdmission = getActivationAdmission?.Invoke(actorType);
        activationAdmission?.Acquire($"ACTOR '{actorId}'");
        try
        {
            await using var scope = services.CreateAsyncScope();
            EnsureNativeActorRef(
                state,
                actorId,
                createRequest,
                reservedGeneration,
                reservedAuthorityOwnerGeneration);
            var context = ensureActorContext(state);
            try
            {
                var factory = (IZLinkActorFactory)scope.ServiceProvider.GetRequiredService(factoryType);
                var actor = await factory.CreateAsync(context, cancellationToken)
                    .ConfigureAwait(false);
                if (actor is null)
                    throw new InvalidOperationException($"Actor factory '{factoryType}' returned null.");

                if (!ReferenceEquals(actor.Context, context))
                    throw new InvalidOperationException(
                        $"Actor factory '{factoryType}' must return an Actor that exposes the provided context.");

                bindActorContext(actor, state);
                return actor;
            }
            catch (Exception activationFailure)
            {
                await DestroyStagedNativeActorAsync(state, activationFailure).ConfigureAwait(false);
                throw;
            }
        }
        finally
        {
            activationAdmission?.Release();
        }
    }

    private async ValueTask DestroyStagedNativeActorAsync(
        ZLinkActorRuntimeState state,
        Exception activationFailure)
    {
        if (state.NativeActorRef is not { } nativeActor) return;

        var node = getActorSpotNode();
        if (node is null) return;

        try
        {
            await node.DestroyActorAsync(
                    nativeActor,
                    runtime.Registration.DefaultRequestTimeout,
                    CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch (Exception cleanupFailure)
        {
            throw new AggregateException(activationFailure, cleanupFailure);
        }
    }

    private void EnsureNativeActorRef(
        ZLinkActorRuntimeState state,
        string actorId,
        ZLinkMessage createRequest,
        ulong? reservedGeneration = null,
        ulong? reservedAuthorityOwnerGeneration = null)
    {
        var node = getActorSpotNode();
        if (node is null || state.NativeActorRef is not null) return;

        var existingRef = node.ActorLookup(actorId);
        if (existingRef is not null)
        {
            state.BindNativeActorRef(existingRef.Value);
            return;
        }

        using var nativeCreateRequest = createRequest.ToRawMessage(runtime.Registration.Codecs);
        state.BindNativeActorRef(reservedGeneration is { } generation
            ? node.CreateReservedActor(
                actorId,
                generation,
                reservedAuthorityOwnerGeneration
                ?? throw new InvalidOperationException(
                    "Reserved Actor creation requires an authority owner generation."),
                nativeCreateRequest)
            : node.CreateActor(actorId, nativeCreateRequest));
    }

    private async ValueTask PublishActorRefOrCompensateAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        ZLinkBackendActorRef nativeActor,
        IZLinkActorLocationLifecycle lifecycle,
        CancellationToken cancellationToken)
    {
        try
        {
            await lifecycle.PublishActorRefAsync(
                    actorId,
                    nativeActor.ToNative(
                        Host.ZLinkActorDrainCoordinator.ResolveMeshName(
                            runtime.Registration,
                            state.ActorType
                            ?? throw new InvalidOperationException(
                                $"Actor '{actorId}' does not have a stable type."))
                        ?? throw new InvalidOperationException(
                            $"Actor '{actorId}' does not have an owner Mesh.")),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception publishFailure)
        {
            await state.ExecuteLockedAsync(
                    state.BeginTeardown,
                    CancellationToken.None)
                .ConfigureAwait(false);

            try
            {
                await teardownActor(state, nativeActor, CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (Exception cleanupFailure)
            {
                runtime.RunDetached(
                    "actor-creation-compensation",
                    ct => ReconcileCreationCompensationAsync(
                        state,
                        actorId,
                        nativeActor,
                        ct));
                throw new AggregateException(publishFailure, cleanupFailure);
            }
            throw;
        }
    }

    private async ValueTask ReconcileCreationCompensationAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        ZLinkBackendActorRef nativeActor,
        CancellationToken cancellationToken)
    {
        await ZLinkReconciliationRunner.RunAsync(
                token => teardownActor(state, nativeActor, token),
                exception => ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"actor creation compensation retry for '{actorId}': {exception.Message}"),
                cancellationToken,
                static exception => exception is OperationCanceledException)
            .ConfigureAwait(false);
    }
}
