package systems.zlink.framework.kotlin

import java.nio.file.Files
import java.nio.file.Path
import java.time.Duration
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import java.util.logging.Handler
import java.util.logging.LogRecord
import java.util.logging.Logger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.selects.select
import kotlinx.coroutines.withTimeout
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertNotEquals
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.framework.spots.ZLinkSpotClosingContext
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle

class KotlinInstanceSpotCloseContractTest {
    @Test
    fun suspendingClosingReplaysTheOriginalIntentRequestOnceInTheNewIncarnation() = runBlocking {
        withTimeout(TEST_TIMEOUT.toMillis()) {
            val suffix = UUID.randomUUID().toString()
            val spotId = "kotlin-close-$suffix"
            val nodeRid = RoutingId.from("kotlin-close-node-$suffix")
            val observation = CloseObservation()
            currentObservation = observation
            val options = DefaultZLinkFrameworkOptions()
            options.addLocationStore(ZLinkInMemoryLocationStore())
            options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL)
            val node = options.addRouteMesh(MESH_NAME)
            node.listen("inproc://kotlin-close-$suffix").setRoutingId(nodeRid)
            node.objects().server().addInstanceSpotFactory(
                INSTANCE_TYPE,
                ClosingInstance::class.java,
            ) { factory ->
                factory.disableRelocation()
            }
            val lifecycle =
                ZLinkFrameworkLifecycle(
                    options,
                    ZLinkJavaBackendAdapterFactory(),
                    ZLinkHandlerActivator.reflection(),
                )
            val logger = Logger.getLogger(FLOW_LOGGER)
            val flow = OwnerArrivalLog(spotId, suffix)
            logger.addHandler(flow)
            try {
                lifecycle.start()
                val outbound =
                    (lifecycle as systems.zlink.framework.channels.ZLinkRouteClient).kotlin()
                val originalGeneration =
                    outbound
                        .requestToSpot(spotId, InitialProbe("initial"), Long::class)
                        .instanceSpot(INSTANCE_TYPE)
                        .inMesh(MESH_NAME)
                        .timeout(TEST_TIMEOUT)
                        .await()
                outbound
                    .sendToSpot(spotId, CloseProbe("close"))
                    .instanceSpot(INSTANCE_TYPE)
                    .inMesh(MESH_NAME)
                    .await()
                select<Unit> {
                    observation.closingEntered.onAwait {}
                    observation.closeCompleted.onAwait { closed ->
                        throw AssertionError("Close settled before OnClosing: $closed")
                    }
                }
                val completions = AtomicInteger()
                val pending =
                    async(start = CoroutineStart.UNDISPATCHED) {
                        val generation =
                            outbound
                                .requestToSpot(spotId, PendingProbe("pending"), Long::class)
                                .instanceSpot(INSTANCE_TYPE)
                                .inMesh(MESH_NAME)
                                .timeout(TEST_TIMEOUT)
                                .await()
                        completions.incrementAndGet()
                        generation
                    }
                assertTrue(flow.ownerArrived.await().isNotBlank())
                assertFalse(pending.isCompleted)
                assertEquals(0, observation.pendingCalls.get())
                observation.closingRelease.complete(Unit)
                val newGeneration = pending.await()
                assertTrue(observation.closeCompleted.await())
                assertNotEquals(originalGeneration, newGeneration)
                assertEquals(listOf(originalGeneration, newGeneration), observation.initializations)
                assertEquals(1, observation.closingCalls.get())
                assertEquals(1, observation.pendingCalls.get())
                assertEquals(1, completions.get())
            } finally {
                observation.closingRelease.complete(Unit)
                lifecycle.stop()
                logger.removeHandler(flow)
                flow.close()
            }
        }
    }

    class ClosingInstance(override val context: ZLinkInstanceSpotContext) :
        ZLinkSuspendingInstanceSpot() {
        val observation = currentObservation

        override fun configure() {
            context.handlers().addPacket<InitialHandler>()
            context.handlers().addPacket<PendingHandler>()
            context.handlers().addPacket<CloseHandler>()
        }

        override suspend fun onInitializeSuspending() {
            observation.initializations.add(context.objectGeneration())
        }

        override suspend fun onClosingSuspending(context: ZLinkSpotClosingContext) {
            observation.closingCalls.incrementAndGet()
            observation.closingEntered.complete(Unit)
            observation.closingRelease.await()
        }
    }

    class InitialHandler : ZLinkSuspendingSpotRequestHandler<ClosingInstance, InitialProbe, Long> {
        override suspend fun handle(spot: ClosingInstance, request: InitialProbe): Long =
            spot.context.objectGeneration()
    }

    class PendingHandler : ZLinkSuspendingSpotRequestHandler<ClosingInstance, PendingProbe, Long> {
        override suspend fun handle(spot: ClosingInstance, request: PendingProbe): Long {
            spot.observation.pendingCalls.incrementAndGet()
            return spot.context.objectGeneration()
        }
    }

    class CloseHandler : ZLinkSuspendingSpotPacketHandler<ClosingInstance, CloseProbe> {
        override suspend fun handle(spot: ClosingInstance, message: CloseProbe) {
            spot.context.close().whenComplete { closed, failure ->
                if (failure == null) {
                    spot.observation.closeCompleted.complete(closed)
                } else {
                    spot.observation.closeCompleted.completeExceptionally(failure)
                }
            }
        }
    }

    data class InitialProbe(val value: String)

    data class PendingProbe(val value: String)

    data class CloseProbe(val value: String)

    class CloseObservation {
        val closingEntered = CompletableDeferred<Unit>()
        val closingRelease = CompletableDeferred<Unit>()
        val closeCompleted = CompletableDeferred<Boolean>()
        val closingCalls = AtomicInteger()
        val pendingCalls = AtomicInteger()
        val initializations = CopyOnWriteArrayList<Long>()
    }

    private class OwnerArrivalLog(private val spotId: String, suffix: String) : Handler() {
        val ownerArrived = CompletableDeferred<String>()
        private val logPath = Path.of("build", "kotlin-close-$suffix.flow")
        private val writer =
            Files.createDirectories(logPath.parent).let { Files.newBufferedWriter(logPath) }

        override fun publish(record: LogRecord) {
            val line = record.message ?: return
            synchronized(writer) {
                writer.appendLine(line)
                writer.flush()
            }
            val fields =
                line
                    .split(' ')
                    .mapNotNull { part ->
                        val separator = part.indexOf('=')
                        if (separator < 0) null
                        else part.substring(0, separator) to part.substring(separator + 1)
                    }
                    .toMap()
            if (
                fields["phase"] == "received" &&
                    fields["spot"] == spotId &&
                    fields["packet"] == PendingProbe::class.java.simpleName &&
                    !fields["corr"].isNullOrEmpty()
            ) {
                ownerArrived.complete(fields.getValue("corr"))
            }
        }

        override fun flush() = synchronized(writer) { writer.flush() }

        override fun close() = synchronized(writer) { writer.close() }
    }

    companion object {
        private val TEST_TIMEOUT = Duration.ofSeconds(10)
        private const val MESH_NAME = "kotlin-close-mesh"
        private const val INSTANCE_TYPE = "kotlin-close-instance"
        private const val FLOW_LOGGER =
            "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer"
        private lateinit var currentObservation: CloseObservation
    }
}
