package systems.zlink.tutorial.server.channel;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.tutorial.shared.Contracts;

// --8<-- [start:clientserver-handler]
// A ClientServer channel handler is written exactly like a RouteMesh one. Only
// the way the caller reaches it differs.
public final class IssueSessionTicketHandler
    implements ZLinkRequestHandler<Contracts.IssueSessionTicket, Contracts.SessionTicket> {

    @Override
    public CompletionStage<Contracts.SessionTicket> handle(
        Contracts.IssueSessionTicket request,
        ZLinkMessageContext context) {
        return CompletableFuture.completedFuture(
            new Contracts.SessionTicket("ticket-" + request.playerId()));
    }
}
// --8<-- [end:clientserver-handler]
