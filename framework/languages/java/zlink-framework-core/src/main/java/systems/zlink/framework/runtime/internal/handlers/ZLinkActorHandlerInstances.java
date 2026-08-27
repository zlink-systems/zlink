package systems.zlink.framework.runtime.internal.handlers;
import java.util.Objects;

import java.util.IdentityHashMap;
import java.util.Map;
import java.util.concurrent.CompletionException;
import java.util.function.Supplier;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/**
 * Internal bridge between Actor lifecycle ownership and Spot dispatch.
 *
 * <p>The bridge intentionally exposes only handler resolution. Actor runtime
 * retains the sole ability to close the activation owner.</p>
 */
public final class ZLinkActorHandlerInstances {
    private static final Map<ZLinkActor, ZLinkHandlerInstanceOwner> OWNERS =
        new IdentityHashMap<>();
    private static final ZLinkStateLane STATE_LANE = new ZLinkStateLane();

    private ZLinkActorHandlerInstances() {
    }

    public static void bind(
        ZLinkActor actor,
        ZLinkHandlerInstanceOwner owner) {
        inStateLane(() -> {
            Objects.requireNonNull(actor, "actor");
            Objects.requireNonNull(owner, "owner");
            ZLinkHandlerInstanceOwner previous = OWNERS.get(actor);
            if (previous != null && previous != owner) {
                throw new IllegalStateException(
                    "Actor handler owner is already bound");
            }
            OWNERS.put(actor, owner);
            return null;
        });
    }

    public static void unbind(
        ZLinkActor actor,
        ZLinkHandlerInstanceOwner owner) {
        inStateLane(() -> {
            if (actor != null) {
                OWNERS.remove(actor, owner);
            }
            return null;
        });
    }

    public static Object instance(
        ZLinkActor actor,
        Class<?> handlerType) {
        return inStateLane(() -> {
            ZLinkHandlerInstanceOwner owner = OWNERS.get(
                Objects.requireNonNull(actor, "actor"));
            if (owner == null) {
                throw new IllegalStateException(
                    "Actor handler owner is unavailable: "
                        + actor.context().actorId());
            }
            return owner;
        }).instance(handlerType);
    }

    private static <T> T inStateLane(Supplier<T> work) {
        try {
            return STATE_LANE.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }
}
