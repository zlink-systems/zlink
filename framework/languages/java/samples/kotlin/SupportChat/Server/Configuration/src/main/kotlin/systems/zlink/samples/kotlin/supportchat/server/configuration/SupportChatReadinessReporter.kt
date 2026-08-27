package systems.zlink.samples.kotlin.supportchat.server.configuration

import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.slf4j.LoggerFactory
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.ApplicationRunner
import systems.zlink.framework.errors.ZLinkConfigurationException
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime

class SupportChatReadinessReporter(
    private val nodeId: String,
    private val meshes: ZLinkRouteMeshRuntime,
) : ApplicationRunner, AutoCloseable {
    private val reporter = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "supportchat-readiness").apply { isDaemon = true }
    }

    override fun run(args: ApplicationArguments) {
        reporter.scheduleWithFixedDelay(::report, 0, 100, TimeUnit.MILLISECONDS)
    }

    private fun report() {
        try {
            val mesh = meshes.snapshot(SampleNames.SupportSpotDiscovery)
            if (!mesh.isReady || mesh.readyPeerCount == 0) return

            logger.info(
                "supportchat-ready kind=spot-route node={} mesh={}",
                nodeId,
                SampleNames.SupportSpotDiscovery,
            )
            reporter.shutdown()
        } catch (_: IllegalStateException) {
            // The public runtime view is not available until Framework startup completes.
        } catch (_: ZLinkConfigurationException) {
            // The public runtime view is not available until Framework startup completes.
        }
    }

    override fun close() {
        reporter.shutdownNow()
    }

    private companion object {
        private val logger = LoggerFactory.getLogger(SupportChatReadinessReporter::class.java)
    }
}
