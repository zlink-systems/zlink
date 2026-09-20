package systems.zlink.tutorial.server.channel

import org.slf4j.LoggerFactory
import systems.zlink.framework.channels.ZLinkPublishMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingPublishHandler
import systems.zlink.tutorial.shared.MaintenanceNotice

// --8<-- [start:fanout-handler]
// Receives what any publisher on this channel sends. The publisher does not know
// this node exists, so adding or removing a subscriber changes nothing there.
//
// No group annotation: the fanout builder takes the handler class directly, so
// this one is registered by type.
class MaintenanceNoticeSubscriber : ZLinkSuspendingPublishHandler<MaintenanceNotice> {

    private val log = LoggerFactory.getLogger(MaintenanceNoticeSubscriber::class.java)

    override suspend fun handle(message: MaintenanceNotice, context: ZLinkPublishMessageContext) {
        log.info("maintenance notice: {}", message.message)
    }
}
// --8<-- [end:fanout-handler]
