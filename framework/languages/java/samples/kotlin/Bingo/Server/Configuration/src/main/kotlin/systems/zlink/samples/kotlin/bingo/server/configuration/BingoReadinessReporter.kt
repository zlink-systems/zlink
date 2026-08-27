package systems.zlink.samples.kotlin.bingo.server.configuration

import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import org.slf4j.LoggerFactory
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.ApplicationRunner
import systems.zlink.framework.errors.ZLinkConfigurationException
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime

class BingoReadinessReporter private constructor(
    private val checks: List<ReadinessCheck>,
) : ApplicationRunner, AutoCloseable {
    private val reporter: ScheduledExecutorService =
        Executors.newSingleThreadScheduledExecutor { runnable ->
            Thread(runnable, "bingo-readiness").apply { isDaemon = true }
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
        private val logger = LoggerFactory.getLogger(BingoReadinessReporter::class.java)

        fun api(
            topology: SampleTopology,
            meshes: ZLinkRouteMeshRuntime,
        ): BingoReadinessReporter {
            val node = "api-${topology.apiNode}"
            return BingoReadinessReporter(
                listOf(
                    ReadinessCheck(
                        "bingo-ready kind=mesh-route node=$node mesh=matchmaking",
                    ) { meshes.snapshot(SampleNames.MatchmakingMesh).readyPeerCount > 0 },
                    ReadinessCheck(
                        "bingo-ready kind=mesh-route node=$node mesh=room",
                    ) { meshes.snapshot(SampleNames.Mesh).readyPeerCount > 0 },
                ),
            )
        }

        fun play(
            topology: SampleTopology,
            meshes: ZLinkRouteMeshRuntime,
        ): BingoReadinessReporter {
            val node = "play-${topology.playNode}"
            val peer = "play-${if (topology.playNode == "a") "b" else "a"}"
            return BingoReadinessReporter(
                listOf(
                    ReadinessCheck(
                        "bingo-ready kind=peer-route node=$node peer=$peer",
                    ) { meshes.snapshot(SampleNames.Mesh).readyPeerCount >= 5 },
                ),
            )
        }

        fun session(
            topology: SampleTopology,
            meshes: ZLinkRouteMeshRuntime,
        ): BingoReadinessReporter {
            val node = "session-${topology.sessionNode}"
            return BingoReadinessReporter(
                listOf(
                    ReadinessCheck(
                        "bingo-ready kind=mesh-route node=$node mesh=room",
                    ) { meshes.snapshot(SampleNames.Mesh).readyPeerCount > 0 },
                ),
            )
        }
    }
}
