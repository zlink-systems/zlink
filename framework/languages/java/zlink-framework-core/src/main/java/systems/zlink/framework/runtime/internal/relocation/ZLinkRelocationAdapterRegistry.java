package systems.zlink.framework.runtime.internal.relocation;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorBaseDeltaRelocationAdapter;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.spots.ZLinkSpotBaseDeltaRelocationAdapter;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;

public final class ZLinkRelocationAdapterRegistry {
    private final Map<String, Class<?>> actorAdapters;
    private final Map<String, Class<?>> spotAdapters;
    private final ZLinkHandlerActivator activator;

    public ZLinkRelocationAdapterRegistry(
        ZLinkFrameworkRegistration registration,
        ZLinkHandlerActivator activator) {
        this.activator = activator;
        Map<String, Class<?>> actors = new LinkedHashMap<>();
        Map<String, Class<?>> spots = new LinkedHashMap<>();
        for (MeshNodeRegistration node : registration.meshNodes()) {
            node.relocatableActorFactories().forEach((stableType, factory) ->
                addPreservedState(actors, stableType, factory.relocationPolicy()));
            node.relocatableSpotFactories().forEach((stableType, factory) ->
                addPreservedState(spots, stableType, factory.relocationPolicy()));
            node.relocatableInstanceSpotFactories().forEach((stableType, factory) ->
                addPreservedState(spots, stableType, factory.relocationPolicy()));
        }
        actorAdapters = Map.copyOf(actors);
        spotAdapters = Map.copyOf(spots);
    }

    public boolean hasAdapters() {
        return !actorAdapters.isEmpty() || !spotAdapters.isEmpty();
    }

    public boolean hasActorAdapter(String stableType) {
        return actorAdapters.containsKey(stableType);
    }

    public boolean hasSpotAdapter(String stableType) {
        return spotAdapters.containsKey(stableType);
    }

    /**
     * True when the registered Actor adapter class opts into the optional
     * base/delta capture capability (spec 15 §5) — an {@code instanceof}
     * class-level probe, decided once at registration scan time.
     */
    public boolean hasActorBaseDeltaAdapter(String stableType) {
        Class<?> adapterType = actorAdapters.get(stableType);
        return adapterType != null
            && ZLinkActorBaseDeltaRelocationAdapter.class
                .isAssignableFrom(adapterType);
    }

    /** Spot analog of {@link #hasActorBaseDeltaAdapter(String)}. */
    public boolean hasSpotBaseDeltaAdapter(String stableType) {
        Class<?> adapterType = spotAdapters.get(stableType);
        return adapterType != null
            && ZLinkSpotBaseDeltaRelocationAdapter.class
                .isAssignableFrom(adapterType);
    }

    @SuppressWarnings("unchecked")
    public CompletionStage<byte[]> captureActor(
        String stableType,
        ZLinkActor actor,
        ZLinkRelocationCancellation cancellation) {
        ZLinkActorRelocationAdapter<ZLinkActor> adapter =
            (ZLinkActorRelocationAdapter<ZLinkActor>) create(
                actorAdapters,
                stableType,
                "Actor");
        return adapter.capture(actor, cancellation);
    }

    @SuppressWarnings("unchecked")
    public CompletionStage<Void> restoreActor(
        String stableType,
        ZLinkActor actor,
        byte[] state,
        ZLinkRelocationCancellation cancellation) {
        ZLinkActorRelocationAdapter<ZLinkActor> adapter =
            (ZLinkActorRelocationAdapter<ZLinkActor>) create(
                actorAdapters,
                stableType,
                "Actor");
        return adapter.restore(actor, state, cancellation);
    }

    @SuppressWarnings("unchecked")
    public CompletionStage<byte[]> captureSpot(
        String stableType,
        Object spot,
        ZLinkRelocationCancellation cancellation) {
        ZLinkSpotRelocationAdapter<Object> adapter =
            (ZLinkSpotRelocationAdapter<Object>) create(
                spotAdapters,
                stableType,
                "Spot");
        return adapter.capture(spot, cancellation);
    }

    @SuppressWarnings("unchecked")
    public CompletionStage<Void> restoreSpot(
        String stableType,
        Object spot,
        byte[] state,
        ZLinkRelocationCancellation cancellation) {
        ZLinkSpotRelocationAdapter<Object> adapter =
            (ZLinkSpotRelocationAdapter<Object>) create(
                spotAdapters,
                stableType,
                "Spot");
        return adapter.restore(spot, state, cancellation);
    }

    public CompletionStage<byte[]> captureActorBase(
        String stableType,
        ZLinkActor actor,
        ZLinkRelocationCancellation cancellation) {
        return actorBaseDelta(stableType).captureBase(actor, cancellation);
    }

    public CompletionStage<byte[]> captureActorDelta(
        String stableType,
        ZLinkActor actor,
        ZLinkRelocationCancellation cancellation) {
        return actorBaseDelta(stableType).captureDelta(actor, cancellation);
    }

    public CompletionStage<Void> restoreActorBase(
        String stableType,
        ZLinkActor actor,
        byte[] base,
        ZLinkRelocationCancellation cancellation) {
        return actorBaseDelta(stableType)
            .restoreBase(actor, base, cancellation);
    }

    public CompletionStage<Void> applyActorDelta(
        String stableType,
        ZLinkActor actor,
        byte[] delta,
        ZLinkRelocationCancellation cancellation) {
        return actorBaseDelta(stableType)
            .applyDelta(actor, delta, cancellation);
    }

    public CompletionStage<byte[]> captureSpotBase(
        String stableType,
        Object spot,
        ZLinkRelocationCancellation cancellation) {
        return spotBaseDelta(stableType).captureBase(spot, cancellation);
    }

    public CompletionStage<byte[]> captureSpotDelta(
        String stableType,
        Object spot,
        ZLinkRelocationCancellation cancellation) {
        return spotBaseDelta(stableType).captureDelta(spot, cancellation);
    }

    public CompletionStage<Void> restoreSpotBase(
        String stableType,
        Object spot,
        byte[] base,
        ZLinkRelocationCancellation cancellation) {
        return spotBaseDelta(stableType).restoreBase(spot, base, cancellation);
    }

    public CompletionStage<Void> applySpotDelta(
        String stableType,
        Object spot,
        byte[] delta,
        ZLinkRelocationCancellation cancellation) {
        return spotBaseDelta(stableType).applyDelta(spot, delta, cancellation);
    }

    @SuppressWarnings("unchecked")
    private ZLinkActorBaseDeltaRelocationAdapter<ZLinkActor> actorBaseDelta(
        String stableType) {
        Object adapter = create(actorAdapters, stableType, "Actor");
        if (!(adapter instanceof ZLinkActorBaseDeltaRelocationAdapter)) {
            throw new IllegalArgumentException(
                "Actor relocation adapter does not implement the base/delta "
                    + "capability: " + stableType);
        }
        return (ZLinkActorBaseDeltaRelocationAdapter<ZLinkActor>) adapter;
    }

    @SuppressWarnings("unchecked")
    private ZLinkSpotBaseDeltaRelocationAdapter<Object> spotBaseDelta(
        String stableType) {
        Object adapter = create(spotAdapters, stableType, "Spot");
        if (!(adapter instanceof ZLinkSpotBaseDeltaRelocationAdapter)) {
            throw new IllegalArgumentException(
                "Spot relocation adapter does not implement the base/delta "
                    + "capability: " + stableType);
        }
        return (ZLinkSpotBaseDeltaRelocationAdapter<Object>) adapter;
    }

    private Object create(
        Map<String, Class<?>> adapters,
        String stableType,
        String kind) {
        Class<?> adapterType = adapters.get(stableType);
        if (adapterType == null) {
            throw new IllegalArgumentException(
                kind + " relocation adapter is not registered: " + stableType);
        }
        return activator.create(adapterType);
    }

    private static void addPreservedState(
        Map<String, Class<?>> adapters,
        String stableType,
        RelocationPolicy policy) {
        if (policy
            instanceof RelocationPolicy.PreserveState
                preserveState) {
            adapters.put(stableType, preserveState.adapterClass());
        }
    }
}
