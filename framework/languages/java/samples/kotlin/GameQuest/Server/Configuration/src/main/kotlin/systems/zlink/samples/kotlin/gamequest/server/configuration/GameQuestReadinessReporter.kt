package systems.zlink.samples.kotlin.gamequest.server.configuration

import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.slf4j.LoggerFactory
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.ApplicationRunner
import systems.zlink.framework.errors.ZLinkConfigurationException
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime

class GameQuestReadinessReporter private constructor(
    private val evidence: String,
    private val ready: () -> Boolean,
) : ApplicationRunner, AutoCloseable {
    private val reporter = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "gamequest-readiness").apply { isDaemon = true }
    }

    @Volatile
    private var reported = false

    override fun run(args: ApplicationArguments) {
        reporter.scheduleWithFixedDelay(::reportIfReady, 0, 100, TimeUnit.MILLISECONDS)
    }

    private fun reportIfReady() {
        if (reported) return
        try {
            if (ready()) {
                logger.info(evidence)
                reported = true
                reporter.shutdown()
            }
        } catch (_: IllegalStateException) {
            // The public runtime view is not available until Framework startup completes.
        } catch (_: ZLinkConfigurationException) {
            // The public runtime view is not available until Framework startup completes.
        }
    }

    override fun close() {
        reporter.shutdownNow()
    }

    companion object {
        private val logger = LoggerFactory.getLogger(GameQuestReadinessReporter::class.java)

        fun api(
            nodeId: String,
            meshes: ZLinkRouteMeshRuntime,
        ): GameQuestReadinessReporter = GameQuestReadinessReporter(
            "gamequest-ready kind=spot-route node=$nodeId mesh=${SampleNames.PlayerQuestMesh}",
        ) { meshes.snapshot(SampleNames.PlayerQuestMesh).isReady }
    }
}
