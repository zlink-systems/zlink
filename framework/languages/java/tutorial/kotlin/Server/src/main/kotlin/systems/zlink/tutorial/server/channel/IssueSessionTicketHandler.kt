package systems.zlink.tutorial.server.channel

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.tutorial.shared.IssueSessionTicket
import systems.zlink.tutorial.shared.SessionTicket

// --8<-- [start:clientserver-handler]
// A ClientServer channel handler is written exactly like a RouteMesh one. Only
// the way the caller reaches it differs, and the group it is registered under.
@ZLinkHandlerGroup(HandlerGroups.TICKETING)
class IssueSessionTicketHandler : ZLinkSuspendingRequestHandler<IssueSessionTicket, SessionTicket> {

    override suspend fun handle(
        request: IssueSessionTicket,
        context: ZLinkMessageContext,
    ): SessionTicket = SessionTicket("ticket-${request.playerId}")
}
// --8<-- [end:clientserver-handler]
