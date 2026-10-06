package systems.zlink.framework.perf.servers.publisher

import java.nio.file.Path
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.future.await
import kotlinx.coroutines.launch
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
    @Volatile private var windowSuccess = FanoutSupport.SequenceBitSet()

    init {
        measurement.onReset {
            measuredBase = issued.get()
            windowSuccess = FanoutSupport.SequenceBitSet()
        }
        measurement.messageTypes(listOf(arrayOf("event", "PerfPublishEvent")))
        measurement.enrichSnapshot(::enrich)
    }

    companion object {
        fun run(config: RoleConfig, cellDirectory: Path) {
            require(config.role() == "publisher" && config.source())
            val app =
                ServerApplication.create(config)
                    .configure { options ->
                        options
                            .addFanoutChannel(config.channelName())
                            .setRoutingIdPrefix("perf-publisher")
                            .enablePublisher(config.transportEndpoints().get("fanout"))
                    }
                    .bean(ObjectsReadiness::class.java) {
                        ObjectsReadiness(false, "The Publisher host is not Ready yet.")
                    }
                    .bean(CellDirectory::class.java) { CellDirectory(cellDirectory) }
                    .bean(PubSubFanoutEchoScenario::class.java)
                    .workload(PubSubFanoutEchoScenario::class.java, PubSubFanoutEchoScenario::run)
            app.start().getBean(PubSubFanoutEchoScenario::class.java).prepare().exceptionally {
                error ->
                app.measurement().recordDiagnostic(error)
                null
            }
        }
    }

    fun prepare() = completionStage {
        Polling.until(
                { runtime.getObject().status().isReady() },
                10,
                config.workload().setupTimeoutMs().toLong(),
            )
            .await()
        val status = runtime.getObject().status()
        readiness.set(
            true,
            "",
            listOf(
                Evidence.of(
                    "publisherHostReady",
                    "ZLinkFrameworkRuntime.status",
                    mapOf(
                        "state" to status.state(),
                        "isReady" to status.isReady(),
                        "acceptingWork" to status.acceptingWork(),
                    ),
                )
            ),
        )
    }

    fun run() = completionStage {
        coroutineScope {
            repeat(config.workload().logicalStreams()) { stream ->
                launch(Dispatchers.IO) {
                    while (measurement.canIssue()) {
                        val warmup = measurement.resetSeq() == "0"
                        val started = measurement.beginOperation("event")
                        if (started < 0) break
                        val sequence: Long
                        val completedTicks: Long
                        try {
                            sequence = issued.incrementAndGet()
                            val message =
                                PerfPublishEvent(
                                    config.runId(),
                                    config.cellId(),
                                    measurement.resetSeq(),
                                    if (warmup) "warmup" else "measured",
                                    DecimalText.of(sequence),
                                    FanoutSupport.TOPIC,
                                    DecimalText.of(started),
                                    PerfClock.DOMAIN,
                                    measurement.pattern().base64(),
                                )
                            fanout
                                .kotlin()
                                .publish(config.channelName(), FanoutSupport.TOPIC, message)
                                .await()
                            completedTicks = PerfClock.now()
                        } catch (error: Throwable) {
                            measurement.completeOperation(started, error)
                            if (error is CancellationException || error !is Exception) throw error
                            continue
                        }
                        val counted = measurement.completeOperation(started, null, completedTicks)
                        if (warmup) {
                            if (measurement.setupEvidence().isEmpty()) {
                                measurement.setupEvidence(
                                    listOf(
                                        Evidence.of(
                                            "warmupMarkerPublished",
                                            "Kotlin ZLinkKotlinFanoutClient.publish(...).await()",
                                            DecimalText.of(sequence),
                                        )
                                    )
                                )
                            }
                        } else if (counted) {
                            windowSuccess.trySet(sequence)
                        }
                    }
                }
            }
        }
    }

    private fun enrich(snapshot: PerfSnapshot) {
        FanoutSupport.applyCommon(snapshot, false)
        FanoutSupport.value(
            snapshot,
            "messages.publishedInWindow",
            DecimalText.of(windowSuccess.count()),
        )
        val seconds = snapshot.window["measuredSeconds"]
        if (seconds is Double && seconds > 0) {
            FanoutSupport.value(
                snapshot,
                "fanout.publishOpsPerSec",
                windowSuccess.count() / seconds,
            )
        } else {
            FanoutSupport.nullValue(
                snapshot,
                "fanout.publishOpsPerSec",
                "PHASE_NOT_STARTED",
                "No measured window has run.",
            )
        }
        snapshot.provenance["fanout"] =
            linkedMapOf(
                "channelName" to config.channelName(),
                "topic" to FanoutSupport.TOPIC,
                "noDrop" to false,
                "publisherSequenceScope" to
                    "one counter per run; warmup and measured ranges are disjoint",
                "sequenceOriginal" to "publisher-sequences.json",
            )
        if (
            !measurement.finalSnapshot() || snapshot.phase != "complete" || snapshot.resetSeq != "1"
        )
            return
        val last = issued.get()
        val base = measuredBase
        val attemptedRanges =
            if (last > base) {
                listOf(FanoutSupport.SequenceRange(DecimalText.of(base + 1), DecimalText.of(last)))
            } else {
                emptyList()
            }
        FanoutSupport.writeOnce(
            sequenceFile,
            FanoutSupport.PublisherSequences(
                config.runId(),
                config.cellId(),
                snapshot.resetSeq,
                "measured",
                attemptedRanges,
                windowSuccess.ranges(),
            ),
        )
    }
}
