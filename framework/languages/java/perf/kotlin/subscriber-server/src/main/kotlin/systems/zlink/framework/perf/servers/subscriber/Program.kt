package systems.zlink.framework.perf.servers.subscriber

import kotlinx.coroutines.Dispatchers
import systems.zlink.framework.channels.ZLinkPublishMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingPublishHandler
import systems.zlink.framework.kotlin.addHandlersFromPackageOf
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.perf.CellDirectory
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.ObjectsReadiness
import systems.zlink.framework.perf.PerfPublishEvent
import systems.zlink.framework.perf.ServerApplication
import systems.zlink.framework.perf.servers.subscriber.FanoutReceipts

private const val HANDLER_GROUP = "perf-kotlin-subscriber"

private class SubscriberHandlerMarker

@ZLinkHandlerGroup(HANDLER_GROUP)
class KotlinPerfFanoutHandler(
    private val measurement: Measurement,
    private val receipts: FanoutReceipts,
) : ZLinkSuspendingPublishHandler<PerfPublishEvent> {
    override suspend fun handle(message: PerfPublishEvent, context: ZLinkPublishMessageContext) {
        measurement.handlerEnter()
        try {
            receipts.record(message)
        } catch (error: RuntimeException) {
            measurement.recordDiagnostic(error)
            throw error
        } finally {
            measurement.handlerExit()
        }
    }
}

fun main(args: Array<String>) {
    val config = ServerApplication.readConfig(args)
    require(config.scenario() == "pubsub-fanout-echo" && config.role() == "subscriber" && !config.source()) {
        "subscriber-server runs the subscriber role of pubsub-fanout-echo."
    }
    val app = ServerApplication.create(config).configure { options ->
        options.addHandlersFromPackageOf<SubscriberHandlerMarker>()
        options.useCoroutineHandlers(Dispatchers.IO)
        options.addFanoutChannel(config.channelName()).enableSubscriber().addHandlerGroup(HANDLER_GROUP)
    }.bean(ObjectsReadiness::class.java) {
        ObjectsReadiness(false, "No Ready publisher is visible to this Subscriber yet.")
    }.bean(CellDirectory::class.java) { CellDirectory(ServerApplication.cellDirectory(args)) }
        .bean(FanoutReceipts::class.java)
    val receipts = app.start().getBean(FanoutReceipts::class.java)
    receipts.prepare()
}
