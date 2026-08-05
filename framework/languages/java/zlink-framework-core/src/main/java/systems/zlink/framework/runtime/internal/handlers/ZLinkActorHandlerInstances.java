package systems.zlink.framework.runtime.internal.handlers;

import java.util.IdentityHashMap;
import java.util.Map;
import systems.zlink.framework.actors.ZLinkActor;

/**
 * Internal bridge between Actor lifecycle ownership and Spot dispatch.
 *
 * <p>The bridge intentionally exposes only handler resolution. Actor runtime
 * retains the sole ability to close the activation owner.</p>
 */
public final class ZLinkActorHandlerInstances {
    private static final Map<ZLinkActor, ZLinkHandlerInstanceOwner> OWNERS =
        new IdentityHashMap<>();

    private ZLinkActorHandlerInstances() {
    }

    public static synchronized void bind(
        ZLinkActor actor,
        ZLinkHandlerInstanceOwner owner) {
        java.util.Objects.requireNonNull(actor, "actor");
        java.util.Objects.requireNonNull(owner, "owner");
        ZLinkHandlerInstanceOwner previous = OWNERS.get(actor);
        if (previous != null && previous != owner) {
            throw new IllegalStateException(
                "Actor handler owner is already bound");
        }
        OWNERS.put(actor, owner);
    }

    public static synchronized void unbind(
        ZLinkActor actor,
        ZLinkHandlerInstanceOwner owner) {
        if (actor != null) {
            OWNERS.remove(actor, owner);
        }
    }

    public static synchronized Object instance(
        ZLinkActor actor,
        Class<?> handlerType) {
        ZLinkHandlerInstanceOwner owner = OWNERS.get(
            java.util.Objects.requireNonNull(actor, "actor"));
        if (owner == null) {
            throw new IllegalStateException(
                "Actor handler owner is unavailable: "
                    + actor.context().actorId());
        }
        return owner.instance(handlerType);
    }
}
