package systems.zlink.framework.runtime.spots

import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Proxy
import java.util.concurrent.CompletableFuture
import org.junit.jupiter.api.Assertions.assertDoesNotThrow
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.assertThrows
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.errors.ZLinkConfigurationException
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotPacketHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotSubscriptionHandler
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.kotlin.addPacket
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
import systems.zlink.framework.spots.ZLinkEntrySpot
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkInstanceSpot
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.framework.spots.ZLinkInstanceSpotHandlerRegistry
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry

class KotlinSpotHandlerRegistrationTest {
    @Test
    fun `Kotlin suspending packet handlers register for Entry and Instance Spots`() {
        val entryHandlers = RecordingSpotRegistry()
        entryHandlers.addHandler<EntryPacketHandler>()
        val instanceHandlers = RecordingInstanceRegistry()
        instanceHandlers.addPacket<InstancePacketHandler>()

        assertDoesNotThrow { load(EntrySpotProbe::class.java, entryHandlers.handlerTypes) }
        assertDoesNotThrow { load(InstanceSpotProbe::class.java, instanceHandlers.handlerTypes) }
    }

    @Test
    fun `Kotlin registry form preserves the Java loader subscription topic error`() {
        val handlers = RecordingSpotRegistry()
        handlers.addHandler<MissingTopicSubscriptionHandler>()

        val failure =
            assertThrows<ZLinkConfigurationException> {
                load(EntrySpotProbe::class.java, handlers.handlerTypes)
            }

        assertEquals(
            "SPOT subscription handler topic is required: ${MissingTopicSubscriptionHandler::class.java.name}",
            failure.message,
        )
    }

    private fun load(spotType: Class<*>, handlerTypes: List<Class<*>>) {
        val scannedHandlers = ZLinkScannedHandlerCatalog(emptyList())
        val actorCatalogType =
            Class.forName("systems.zlink.framework.runtime.spots.ZLinkSpotActorHandlerCatalog")
        val actorCatalog =
            actorCatalogType
                .getDeclaredConstructor(
                    ZLinkScannedHandlerCatalog::class.java,
                    Class.forName("systems.zlink.framework.ZLinkMessageSerializer"),
                )
                .also { it.trySetAccessible() }
                .newInstance(scannedHandlers, null)
        val loaderType =
            Class.forName("systems.zlink.framework.runtime.spots.ZLinkSpotHandlerLoader")
        val loader =
            loaderType
                .getDeclaredConstructor(
                    ZLinkScannedHandlerCatalog::class.java,
                    actorCatalogType,
                    ZLinkHandlerActivator::class.java,
                )
                .also { it.trySetAccessible() }
                .newInstance(scannedHandlers, actorCatalog, ZLinkHandlerActivator { null })
        val timerRegistrarType =
            Class.forName(
                "systems.zlink.framework.runtime.spots.ZLinkSpotHandlerLoader\$ScannedTimerRegistrar"
            )
        val timerRegistrar =
            Proxy.newProxyInstance(javaClass.classLoader, arrayOf(timerRegistrarType)) { _, _, _ ->
                CompletableFuture.completedFuture<Void>(null)
            }
        val load =
            loaderType
                .getDeclaredMethod("load", Class::class.java, List::class.java, timerRegistrarType)
                .also { it.trySetAccessible() }

        try {
            load.invoke(loader, spotType, handlerTypes, timerRegistrar)
        } catch (failure: InvocationTargetException) {
            throw failure.targetException
        }
    }

    private class RecordingSpotRegistry : ZLinkSpotHandlerRegistry {
        val handlerTypes = mutableListOf<Class<*>>()

        override fun addHandler(handlerType: Class<*>) {
            handlerTypes += handlerType
        }
    }

    private class RecordingInstanceRegistry : ZLinkInstanceSpotHandlerRegistry {
        val handlerTypes = mutableListOf<Class<*>>()

        override fun addPacket(handlerType: Class<*>) {
            handlerTypes += handlerType
        }
    }

    private class EntrySpotProbe : ZLinkEntrySpot<ProbeActor> {
        override fun context(): ZLinkEntrySpotContext = error("test only")

        override fun onJoinedActor(actor: ProbeActor) =
            CompletableFuture.completedFuture<Void>(null)

        override fun onLeaveActor(actor: ProbeActor) = CompletableFuture.completedFuture<Void>(null)
    }

    private class InstanceSpotProbe : ZLinkInstanceSpot {
        override fun context(): ZLinkInstanceSpotContext = error("test only")
    }

    private class ProbeActor : ZLinkActor {
        override fun context(): ZLinkActorContext = error("test only")
    }

    private class EntryPacketHandler : ZLinkSuspendingSpotPacketHandler<EntrySpotProbe, Packet> {
        override suspend fun handle(spot: EntrySpotProbe, message: Packet) {}
    }

    private class InstancePacketHandler :
        ZLinkSuspendingSpotPacketHandler<InstanceSpotProbe, Packet> {
        override suspend fun handle(spot: InstanceSpotProbe, message: Packet) {}
    }

    private class MissingTopicSubscriptionHandler :
        ZLinkSuspendingSpotSubscriptionHandler<EntrySpotProbe, Packet> {
        override suspend fun handle(spot: EntrySpotProbe, event: Packet) {}
    }

    private class Packet
}
