package systems.zlink.samples.kotlin.tictactoe.server.configuration

import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.slf4j.LoggerFactory
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.ApplicationRunner
import systems.zlink.framework.errors.ZLinkConfigurationException
import systems.zlink.framework.monitoring.ZLinkPeerState
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime

class TicTacToeReadinessReporter private constructor(
    private val checks: List<ReadinessCheck>,
) : ApplicationRunner, AutoCloseable {
    private val reporter = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "tictactoe-readiness").apply { isDaemon = true }
    }

    override fun run(args: ApplicationArguments) {
        reporter.scheduleWithFixedDelay(::report, 0, 100, TimeUnit.MILLISECONDS)
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
                // The public runtime view is not available until Framework startup completes.
            } catch (_: ZLinkConfigurationException) {
                // The public runtime view is not available until Framework startup completes.
            }
        }
    }

    companion object {
        private val logger = LoggerFactory.getLogger(TicTacToeReadinessReporter::class.java)

        fun api(
            nodeId: String,
            meshes: ZLinkRouteMeshRuntime,
        ): TicTacToeReadinessReporter = TicTacToeReadinessReporter(
            listOf(
                ReadinessCheck(
                    "tictactoe-ready kind=spot-route node=$nodeId mesh=${SampleNames.SpotMesh}",
                ) { meshes.snapshot(SampleNames.SpotMesh).isReady },
            ),
        )

        fun play(
            nodeId: String,
            peerNodeId: String,
            meshes: ZLinkRouteMeshRuntime,
        ): TicTacToeReadinessReporter {
            val peerRoutingId = "tictactoe-play-$peerNodeId"
            return TicTacToeReadinessReporter(
                listOf(
                    ReadinessCheck(
                        "tictactoe-ready kind=peer-route node=$nodeId peer=$peerNodeId",
                    ) {
                        meshes.snapshot(SampleNames.SpotMesh).peers().any { peer ->
                            peer.nodeRid().toString() == peerRoutingId &&
                                peer.state() == ZLinkPeerState.READY
                        }
                    },
                ),
            )
        }
    }
}
