package systems.zlink.samples.kotlin.zoneworld.server.zone;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;

import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.Map;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeCensus;
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.kotlin.zoneworld.server.configuration.SampleTopology;

final class ZoneSpotResidentIdentityTest {
    @Test
    void lateOldIncarnationCallbacksCannotRemoveCurrentResident() throws Exception {
        ZoneSpot spot = new ZoneSpot(
            proxy(ZLinkSpotContext.class, "spotId", "zone-nw"),
            new NodeMaintenanceState(),
            new NodeCensus(),
            proxy(ZLinkActorClient.class, null, null),
            new SampleTopology(
                "zone", "zone-node-1", "tcp://127.0.0.1:1", null,
                "redis://127.0.0.1:1", "test:", false, false, false, "", ""));
        PlayerActor old = new PlayerActor(
            "player-1", proxy(ZLinkActorContext.class, null, null));
        PlayerActor current = new PlayerActor(
            "player-1", proxy(ZLinkActorContext.class, null, null));
        Map<String, PlayerActor> residents = residents(spot);

        residents.put(current.getActorId(), current);
        spot.onLeaveActor(old).toCompletableFuture().join();
        assertSame(current, residents.get(current.getActorId()));
        spot.onLeaveActor(current).toCompletableFuture().join();
        assertFalse(residents.containsKey(current.getActorId()));

        residents.put(current.getActorId(), current);
        spot.onDisconnectActor(old).toCompletableFuture().join();
        assertSame(current, residents.get(current.getActorId()));
        spot.onDisconnectActor(current).toCompletableFuture().join();
        assertFalse(residents.containsKey(current.getActorId()));
    }

    @SuppressWarnings("unchecked")
    private static Map<String, PlayerActor> residents(ZoneSpot spot) throws Exception {
        Field field = ZoneSpot.class.getDeclaredField("residents");
        field.setAccessible(true);
        return (Map<String, PlayerActor>) field.get(spot);
    }

    @SuppressWarnings("unchecked")
    private static <T> T proxy(Class<T> type, String methodName, Object value) {
        return (T) Proxy.newProxyInstance(
            type.getClassLoader(),
            new Class<?>[] {type},
            (proxy, method, arguments) -> method.getName().equals(methodName)
                ? value
                : throwUnsupported(method.getName()));
    }

    private static Object throwUnsupported(String methodName) {
        throw new UnsupportedOperationException(methodName);
    }
}
