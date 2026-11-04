package systems.zlink.samples.zoneworld.server.zone.spots;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.samples.zoneworld.server.configuration.NodeCensus;
import systems.zlink.samples.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;

import java.lang.reflect.Proxy;
import java.util.Map;

final class ZoneSpotResidentIdentityTest {
    @Test
    void lateOldIncarnationCallbacksCannotRemoveCurrentResident() throws Exception {
        ZoneSpot spot =
                new ZoneSpot(
                        proxy(ZLinkSpotContext.class, "spotId", "zone-nw"),
                        new NodeMaintenanceState(),
                        new NodeCensus(),
                        proxy(ZLinkActorClient.class, null, null),
                        null);
        PlayerActor old = new PlayerActor("player-1", proxy(ZLinkActorContext.class, null, null));
        PlayerActor current =
                new PlayerActor("player-1", proxy(ZLinkActorContext.class, null, null));
        Map<String, PlayerActor> residents = spot.residents;

        residents.put(current.actorId(), current);
        spot.onLeaveActor(old).toCompletableFuture().join();
        assertSame(current, residents.get(current.actorId()));
        spot.onLeaveActor(current).toCompletableFuture().join();
        assertFalse(residents.containsKey(current.actorId()));

        residents.put(current.actorId(), current);
        spot.onDisconnectActor(old).toCompletableFuture().join();
        assertSame(current, residents.get(current.actorId()));
        spot.onDisconnectActor(current).toCompletableFuture().join();
        assertFalse(residents.containsKey(current.actorId()));
    }

    @SuppressWarnings("unchecked")
    private static <T> T proxy(Class<T> type, String methodName, Object value) {
        return (T)
                Proxy.newProxyInstance(
                        type.getClassLoader(),
                        new Class<?>[] {type},
                        (proxy, method, arguments) ->
                                method.getName().equals(methodName)
                                        ? value
                                        : throwUnsupported(method.getName()));
    }

    private static Object throwUnsupported(String methodName) {
        throw new UnsupportedOperationException(methodName);
    }
}
