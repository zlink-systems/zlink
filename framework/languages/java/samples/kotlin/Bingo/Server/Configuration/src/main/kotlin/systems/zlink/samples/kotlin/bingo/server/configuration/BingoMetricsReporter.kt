package systems.zlink.samples.kotlin.bingo.server.configuration

import io.micrometer.core.instrument.MeterRegistry
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Connects the framework instruments to the application's Micrometer registry. */
class BingoMetricsReporter(
    private val registry: MeterRegistry,
    role: String,
) : AutoCloseable {
    private val reporter = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "bingo-$role-metrics").apply { isDaemon = true }
    }

    init {
        reporter.scheduleAtFixedRate(::report, 0, 500, TimeUnit.MILLISECONDS)
    }

    private fun report() {
        registry.meters
            .filter { it.id.name.startsWith("zlink.") }
            .forEach { meter ->
                meter.measure().forEach { measurement ->
                    println(
                        "zlink metric role=${Thread.currentThread().name}" +
                            " name=${meter.id.name}" +
                            " statistic=${measurement.statistic}" +
                            " value=${measurement.value}",
                    )
                }
            }
    }

    override fun close() {
        reporter.shutdownNow()
    }
}
