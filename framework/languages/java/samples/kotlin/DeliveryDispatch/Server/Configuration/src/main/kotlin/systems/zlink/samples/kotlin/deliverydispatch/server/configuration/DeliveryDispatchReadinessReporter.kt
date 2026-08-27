package systems.zlink.samples.kotlin.deliverydispatch.server.configuration

import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import org.slf4j.LoggerFactory
import systems.zlink.framework.errors.ZLinkConfigurationException
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.monitoring.ZLinkPeerState

/** Emits the sample-owned readiness evidence from the public, passive mesh view. */
class DeliveryDispatchReadinessReporter private constructor(
    private val checks: List<ReadinessCheck>,
) : AutoCloseable {
    private val reporter: ScheduledExecutorService =
        Executors.newSingleThreadScheduledExecutor { runnable ->
            Thread(runnable, "deliverydispatch-readiness").apply { isDaemon = true }
        }

    private fun start(): DeliveryDispatchReadinessReporter {
        reporter.scheduleWithFixedDelay(::report, 0, 100, TimeUnit.MILLISECONDS)
        return this
    }

    private fun report() {
        checks.forEach(ReadinessCheck::reportIfReady)
        if (checks.all(ReadinessCheck::reported)) {
            reporter.shutdown()
        }
    }

    override fun close() {
        reporter.shutdownNow()
    }

    private class ReadinessCheck(
        private val evidence: String,
        private val ready: () -> Boolean,
    ) {
        var reported: Boolean = false
            private set

        fun reportIfReady() {
            if (reported) return
            try {
                if (ready()) {
                    logger.info(evidence)
                    reported = true
                }
            } catch (_: IllegalStateException) {
                // The public runtime view is available after Framework startup completes.
            } catch (_: ZLinkConfigurationException) {
                // The public runtime view is available after Framework startup completes.
            }
        }
    }

    companion object {
        private val logger = LoggerFactory.getLogger(DeliveryDispatchReadinessReporter::class.java)

        fun route(
            node: String,
            mesh: String,
            meshes: ZLinkRouteMeshRuntime,
        ): DeliveryDispatchReadinessReporter =
            DeliveryDispatchReadinessReporter(
                listOf(
                    ReadinessCheck("deliverydispatch-ready kind=route node=$node") {
                        meshes.isReady(mesh)
                    },
                ),
            ).start()

        fun dispatch(meshes: ZLinkRouteMeshRuntime): DeliveryDispatchReadinessReporter =
            DeliveryDispatchReadinessReporter(
                listOf(
                    ReadinessCheck("deliverydispatch-ready kind=route node=${SampleNames.DispatchNode}") {
                        meshes.isReady(SampleNames.CourierSpotMesh)
                    },
                    ReadinessCheck(
                        "deliverydispatch-ready kind=actor-route node=${SampleNames.DispatchNode} " +
                            "target=${SampleNames.CourierNode1}",
                    ) {
                        hasReadyPeer(meshes, SampleNames.CourierNode1)
                    },
                    ReadinessCheck(
                        "deliverydispatch-ready kind=actor-route node=${SampleNames.DispatchNode} " +
                            "target=${SampleNames.CourierNode2}",
                    ) {
                        hasReadyPeer(meshes, SampleNames.CourierNode2)
                    },
                ),
            ).start()

        //  Confirm the named courier node, not a peer count. A count cannot tell which node is
        //  ready, so it passes on the wrong peer and fails when the count assumption drifts.
        private fun hasReadyPeer(meshes: ZLinkRouteMeshRuntime, target: String): Boolean {
            val courierMesh = meshes.snapshot(SampleNames.CourierSpotMesh)
            return courierMesh.peers.any {
                it.nodeRid().toString() == target && it.state() == ZLinkPeerState.READY
            }
        }
    }
}
