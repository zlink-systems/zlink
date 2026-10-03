package systems.zlink.framework.runtime.internal.handlers;

import systems.zlink.framework.actors.ZLinkActor;

import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Objects;

/**
 * Internal bridge between Actor lifecycle ownership and Spot dispatch.
 *
 * <p>The bridge intentionally exposes only handler resolution. Actor runtime retains the sole
 * ability to close the activation owner.
 */
public final class ZLinkActorHandlerInstances {
    private static final Map<ZLinkActor, ZLinkHandlerInstanceOwner> OWNERS =
            Collections.synchronizedMap(new IdentityHashMap<>());

    private ZLinkActorHandlerInstances() {}

    public static void bind(ZLinkActor actor, ZLinkHandlerInstanceOwner owner) {
        Objects.requireNonNull(actor, "actor");
        Objects.requireNonNull(owner, "owner");
        ZLinkHandlerInstanceOwner previous = OWNERS.putIfAbsent(actor, owner);
        if (previous != null && previous != owner) {
            throw new IllegalStateException("Actor handler owner is already bound");
        }
    }

    public static void unbind(ZLinkActor actor, ZLinkHandlerInstanceOwner owner) {
        if (actor != null) {
            OWNERS.remove(actor, owner);
        }
    }

    public static Object instance(ZLinkActor actor, Class<?> handlerType) {
        ZLinkHandlerInstanceOwner owner = OWNERS.get(Objects.requireNonNull(actor, "actor"));
        if (owner == null) {
            throw new IllegalStateException(
                    "Actor handler owner is unavailable: " + actor.context().actorId());
        }
        return owner.instance(handlerType);
    }
}
