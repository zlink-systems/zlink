package systems.zlink.framework.perf.servers.publisher

import java.nio.file.Path
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.future.await
import org.springframework.beans.factory.ObjectProvider
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.perf.CellDirectory
import systems.zlink.framework.perf.DecimalText
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.FanoutSupport
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.PerfClock
import systems.zlink.framework.perf.PerfPublishEvent
import systems.zlink.framework.perf.PerfSnapshot
import systems.zlink.framework.perf.Polling
import systems.zlink.framework.perf.RoleConfig
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.perf.kotlin.completionStage
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime

private class PublishedSets {
    val window = FanoutSupport.SequenceBitSet()
    val settle = FanoutSupport.SequenceBitSet()
}

class PubSubFanoutEchoScenario(
    private val fanout: ZLinkFanoutClient,
    private val runtime: ObjectProvider<ZLinkFrameworkRuntime>,
    private val measurement: Measurement,
    private val readiness: ObjectsReadiness,
    private val cellDirectory: CellDirectory,
) {
    private val config: RoleConfig = measurement.config()
    private val sequenceFile: Path = cellDirectory.path().resolve("publisher-sequences.json")
    private val issued = AtomicLong()
    @Volatile private var measuredBase = 0L
    @Volatile private var sets = PublishedSets()

    init {
        measurement.onReset { measuredBase = issued.get(); sets = PublishedSets() }
        measurement.messageTypes(listOf(arrayOf("event", "PerfPublishEvent")))
        measurement.enrichSnapshot(::enrich)
    }

    companion object {
        fun run(config: RoleConfig, cellDirectory: Path) {
            require(config.role() == "publisher" && config.source())
            val app = ServerApplication.create(config)
                .configure { options ->
                    options.addFanoutChannel(config.channelName()).setRoutingIdPrefix("perf-publisher")
                        .enablePublisher(config.transportEndpoints().get("fanout"))
                }
                .bean(ObjectsReadiness::class.java) { ObjectsReadiness(false, "The Publisher host is not Ready yet.") }
                .bean(CellDirectory::class.java) { CellDirectory(cellDirectory) }
                .bean(PubSubFanoutEchoScenario::class.java)
                .workload(PubSubFanoutEchoScenario::class.java, PubSubFanoutEchoScenario::run)
            app.start().getBean(PubSubFanoutEchoScenario::class.java).prepare()
                .exceptionally { error -> app.measurement().recordDiagnostic(error); null }
        }
    }

    fun prepare() = completionStage {
        Polling.until({ runtime.getObject().status().isReady() }, 10, config.workload().setupTimeoutMs().toLong()).await()
        val status = runtime.getObject().status()
        readiness.set(true, "", listOf(Evidence.of("publisherHostReady", "ZLinkFrameworkRuntime.status", mapOf(
            "state" to status.state(), "isReady" to status.isReady(), "acceptingWork" to status.acceptingWork(),
        ))))
    }

    fun run() = completionStage {
        coroutineScope {
            repeat(config.workload().logicalStreams()) { stream ->
                repeat(config.workload().inflight()) {
                    launch(Dispatchers.IO) {
                        while (measurement.canIssue()) {
                            val started = measurement.beginOperation("event")
                            if (started < 0) break
                            val sequence = issued.incrementAndGet()
                            val warmup = measurement.resetSeq() == "0"
                            val message = PerfPublishEvent(
                                config.runId(), config.cellId(), measurement.resetSeq(),
                                if (warmup) "warmup" else "measured", DecimalText.of(sequence), FanoutSupport.TOPIC,
                                DecimalText.of(started), PerfClock.DOMAIN, measurement.pattern().base64(),
                            )
                            try {
                                fanout.kotlin().publish(config.channelName(), FanoutSupport.TOPIC, message).await()
                                val completed = PerfClock.now()
                                measurement.completeOperation(started, null, completed)
                                if (warmup) {
                                    if (measurement.setupEvidence().isEmpty()) {
                                        measurement.setupEvidence(listOf(Evidence.of("warmupMarkerPublished", "Kotlin ZLinkKotlinFanoutClient.publish(...).await()", message.sequence())))
                                    }
                                } else {
                                    val current = sets
                                    (if (completed < measurement.endTicks()) current.window else current.settle).trySet(sequence)
                                }
                            } catch (error: Exception) {
                                if (error is CancellationException) throw error
                                measurement.completeOperation(started, error)
                            }
                        }
                    }
                }
            }
        }
    }

    private fun enrich(snapshot: PerfSnapshot) {
        val current = sets
        FanoutSupport.applyCommon(snapshot, false)
        FanoutSupport.value(snapshot, "messages.publishedInWindow", DecimalText.of(current.window.count()))
        FanoutSupport.value(snapshot, "messages.settlePublished", DecimalText.of(current.settle.count()))
        FanoutSupport.value(snapshot, "messages.published", DecimalText.of(current.window.count() + current.settle.count()))
        val seconds = snapshot.window["measuredSeconds"]
        if (seconds is Double && seconds > 0) {
            FanoutSupport.value(snapshot, "fanout.publishOpsPerSec", current.window.count() / seconds)
        } else {
            FanoutSupport.nullValue(snapshot, "fanout.publishOpsPerSec", "PHASE_NOT_STARTED", "No measured window has run.")
        }
        snapshot.provenance["fanout"] = linkedMapOf(
            "channelName" to config.channelName(), "topic" to FanoutSupport.TOPIC, "noDrop" to false,
            "publisherSequenceScope" to "one counter per run; warmup and measured ranges are disjoint",
            "sequenceOriginal" to "publisher-sequences.json",
        )
        if (!measurement.finalSnapshot() || snapshot.phase != "complete" || snapshot.resetSeq != "1") return
        val last = issued.get()
        val base = measuredBase
        FanoutSupport.writeOnce(sequenceFile, FanoutSupport.PublisherSequences(
            config.runId(), config.cellId(), snapshot.resetSeq, "measured",
            if (last > base) listOf(FanoutSupport.SequenceRange(DecimalText.of(base + 1), DecimalText.of(last))) else emptyList(),
            current.window.ranges(), current.settle.ranges(),
        ))
    }
}
