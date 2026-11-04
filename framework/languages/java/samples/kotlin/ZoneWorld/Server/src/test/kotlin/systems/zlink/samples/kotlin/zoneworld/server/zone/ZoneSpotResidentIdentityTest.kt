package systems.zlink.samples.kotlin.zoneworld.server.zone

import java.lang.reflect.Proxy
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Test
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeCensus
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeMaintenanceState
import systems.zlink.samples.kotlin.zoneworld.server.configuration.SampleTopology

class ZoneSpotResidentIdentityTest {
    @Test
    fun lateOldIncarnationCallbacksCannotRemoveCurrentResident() {
        val spot =
            ZoneSpot(
                proxy(ZLinkSpotContext::class.java, "spotId", "zone-nw"),
                NodeMaintenanceState(),
                NodeCensus(),
                proxy(ZLinkActorClient::class.java, null, null),
                SampleTopology(
                    "zone",
                    "zone-node-1",
                    "tcp://127.0.0.1:1",
                    null,
                    "redis://127.0.0.1:1",
                    "test:",
                    false,
                    false,
                    false,
                    "",
                    "",
                    1,
                ),
            )
        val old = PlayerActor("player-1", proxy(ZLinkActorContext::class.java, null, null))
        val current = PlayerActor("player-1", proxy(ZLinkActorContext::class.java, null, null))
        val residents = spot.residents

        residents[current.actorId] = current
        spot.onLeaveActor(old).toCompletableFuture().join()
        assertSame(current, residents[current.actorId])
        spot.onLeaveActor(current).toCompletableFuture().join()
        assertFalse(residents.containsKey(current.actorId))

        residents[current.actorId] = current
        spot.onDisconnectActor(old).toCompletableFuture().join()
        assertSame(current, residents[current.actorId])
        spot.onDisconnectActor(current).toCompletableFuture().join()
        assertFalse(residents.containsKey(current.actorId))
    }

    @Suppress("UNCHECKED_CAST")
    private fun <T> proxy(type: Class<T>, methodName: String?, value: Any?): T =
        Proxy.newProxyInstance(type.classLoader, arrayOf(type)) { _, method, _ ->
            if (method.name == methodName) value
            else throw UnsupportedOperationException(method.name)
        } as T
}
