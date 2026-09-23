package systems.zlink.samples.kotlin.zoneworld.server.zone

import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.locks.LockSupport
import java.util.function.Supplier
import kotlinx.coroutines.runBlocking
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Test
import org.springframework.context.SmartLifecycle
import org.springframework.context.support.GenericApplicationContext
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.channels.ZLinkRequestCall
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkSendCall
import systems.zlink.framework.spots.ZLinkSpotRequestCall
import systems.zlink.framework.spots.ZLinkSpotSendCall
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeCensus
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeMaintenanceState
import systems.zlink.samples.kotlin.zoneworld.server.configuration.SampleTopology

internal class ZoneStatusReporterLifecycleTest {
    @Test
    fun runtimePortProducerStopsBeforeRuntimeLifecycle() = runBlocking {
        val runtime = RuntimePort()
        val reporter = reporter(runtime)
        runtime.producer = reporter
        val context = GenericApplicationContext()
        context.registerBean("runtimePort", RuntimePort::class.java, Supplier { runtime })
        context.registerBean(
            "zoneStatusReporter",
            ZoneStatusReporter::class.java,
            Supplier { reporter },
        )
        try {
            context.refresh()
            reporter.reportNow()
            runtime.firstSubmission.get(2, TimeUnit.SECONDS)

            context.close()

            assertEquals(
                0,
                runtime.postStopSubmissions.get(),
                "the status producer must stop before its route runtime port",
            )
            assertEquals(
                0,
                runtime.runningProducerStops.get(),
                "the route runtime must not observe a running status producer while stopping",
            )
        } finally {
            context.close()
        }
    }

    @Test
    fun smartLifecycleStopCompletesItsCallbackExactlyOnce() = runBlocking {
        val runtime = RuntimePort()
        val reporter = reporter(runtime)
        val callbacks = AtomicInteger()
        reporter.start()
        reporter.reportNow()
        runtime.firstSubmission.get(2, TimeUnit.SECONDS)

        reporter.stop { callbacks.incrementAndGet() }

        assertEquals(1, callbacks.get())
        assertFalse(reporter.isRunning())
    }

    @Test
    fun reportNowSubmitsWhileRunning() = runBlocking {
        val runtime = RuntimePort()
        val reporter = reporter(runtime)
        reporter.start()
        reporter.reportNow()
        runtime.firstSubmission.get(2, TimeUnit.SECONDS)
        val submissionsBeforeReportNow = runtime.submissions.get()

        reporter.reportNow()

        assertEquals(submissionsBeforeReportNow + 1, runtime.submissions.get())
        reporter.stop()
    }

    private fun reporter(runtime: RuntimePort) =
        ZoneStatusReporter(
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
            ),
            runtime,
            NodeCensus(),
            NodeMaintenanceState(),
        )

    private class RuntimePort : ZLinkRouteClient, SmartLifecycle {
        val firstSubmission = CompletableFuture<Void>()
        val submissions = AtomicInteger()
        val postStopSubmissions = AtomicInteger()
        val runningProducerStops = AtomicInteger()
        var producer: ZoneStatusReporter? = null
        @Volatile private var running = false
        @Volatile private var stopped = false

        override fun sendToChannel(channelName: String, message: Any): ZLinkSendCall =
            ZLinkSendCall {
                submissions.incrementAndGet()
                if (stopped) {
                    postStopSubmissions.incrementAndGet()
                }
                firstSubmission.complete(null)
                CompletableFuture.completedFuture(null)
            }

        override fun requestToChannel(channelName: String, request: Any): ZLinkRequestCall =
            throw UnsupportedOperationException()

        override fun sendToNode(
            channelName: String,
            target: RoutingId,
            message: Any,
        ): ZLinkSendCall = throw UnsupportedOperationException()

        override fun sendToSpot(spotId: String, message: Any): ZLinkSpotSendCall =
            throw UnsupportedOperationException()

        override fun requestToNode(
            channelName: String,
            target: RoutingId,
            message: Any,
        ): ZLinkRequestCall = throw UnsupportedOperationException()

        override fun requestToSpot(spotId: String, message: Any): ZLinkSpotRequestCall =
            throw UnsupportedOperationException()

        override fun start() {
            running = true
        }

        override fun stop() {
            running = false
            stopped = true
            if (producer?.isRunning() != false) {
                runningProducerStops.incrementAndGet()
            }
            LockSupport.parkNanos(TimeUnit.MILLISECONDS.toNanos(2_200))
        }

        override fun isRunning() = running

        override fun getPhase() = 0
    }
}
