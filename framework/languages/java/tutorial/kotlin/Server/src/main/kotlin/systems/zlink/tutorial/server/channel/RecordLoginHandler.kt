package systems.zlink.tutorial.server.channel

import org.slf4j.LoggerFactory
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler
import systems.zlink.tutorial.shared.RecordLogin

// --8<-- [start:channel-send-handler]
// Handles a one-way message. There is no return value, so the caller is already
// done by the time this runs and cannot observe a failure here.
@ZLinkHandlerGroup(HandlerGroups.PROFILE)
class RecordLoginHandler : ZLinkSuspendingSendHandler<RecordLogin> {

    private val log = LoggerFactory.getLogger(RecordLoginHandler::class.java)

    override suspend fun handle(message: RecordLogin, context: ZLinkMessageContext) {
        log.info("login recorded: {}", message.playerId)
    }
}
// --8<-- [end:channel-send-handler]
