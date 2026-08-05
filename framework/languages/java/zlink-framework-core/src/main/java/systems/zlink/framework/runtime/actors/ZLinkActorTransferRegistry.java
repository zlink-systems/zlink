package systems.zlink.framework.runtime.actors;

import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

final class ZLinkActorTransferRegistry {
    private final Map<String, Class<? extends ZLinkActorRelocationAdapter<?>>> adapters;
    private final ZLinkHandlerActivator handlerFactory;

    ZLinkActorTransferRegistry(
        Map<String, Class<? extends ZLinkActorRelocationAdapter<?>>> adapters,
        ZLinkHandlerActivator handlerFactory) {
        this.adapters = adapters == null ? Map.of() : Map.copyOf(adapters);
        this.handlerFactory = handlerFactory;
    }

    CompletionStage<TransferState> transferOut(String actorType, ZLinkActor actor) {
        Class<? extends ZLinkActorRelocationAdapter<?>> adapterType =
            adapters.get(actorType);
        if (adapterType == null) {
            return CompletableFuture.completedFuture(
                new TransferState(null, ZLinkMessage.empty()));
        }
        return invokeCapture(createAdapter(adapterType), actor)
            .thenApply(state -> {
                if (state == null) {
                    throw new ZLinkConfigurationException(
                        "actor transfer adapter returned a null state: " + adapterType.getName());
                }
                return new TransferState(actorType, ZLinkMessage.of(state.clone()));
            });
    }

    CompletionStage<ZLinkActor> transferIn(
        String actorType,
        String actorId,
        ZLinkActorContext context,
        ZLinkMessage state,
        Class<? extends ZLinkActorFactory> factoryType) {
        Class<? extends ZLinkActorRelocationAdapter<?>> adapterType =
            adapters.get(actorType);
        if (adapterType == null) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkActorFactory factory =
            (ZLinkActorFactory) handlerFactory.create(factoryType);
        return factory.create(context).thenCompose(actor ->
            invokeRestore(
                createAdapter(adapterType),
                actor,
                state.decode(byte[].class)).thenApply(ignored -> actor))
            .thenApply(actor -> {
                if (actor == null || !actorId.equals(actor.context().actorId())) {
                    throw new ZLinkConfigurationException(
                        "actor transfer adapter returned an actor with a different id: "
                            + adapterType.getName());
                }
                return actor;
            });
    }

    private ZLinkActorRelocationAdapter<?> createAdapter(
        Class<? extends ZLinkActorRelocationAdapter<?>> adapterType) {
        return (ZLinkActorRelocationAdapter<?>) handlerFactory.create(adapterType);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private static CompletionStage<byte[]> invokeCapture(
        ZLinkActorRelocationAdapter adapter,
        ZLinkActor actor) {
        return adapter.capture(actor, () -> false);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private static CompletionStage<Void> invokeRestore(
        ZLinkActorRelocationAdapter adapter,
        ZLinkActor actor,
        byte[] state) {
        return adapter.restore(actor, state, () -> false);
    }

    record TransferState(String adapterKey, ZLinkMessage state) {
    }
}
