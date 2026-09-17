using Tutorial.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace Tutorial.Server.Channel;

// --8<-- [start:clientserver-handler]
// A ClientServer channel handler is written exactly like a RouteMesh one. Only
// the way the caller reaches it differs.
public sealed class IssueSessionTicketHandler
    : IZLinkRequestHandler<IssueSessionTicket, SessionTicket>
{
    public ValueTask<SessionTicket> HandleAsync(
        IssueSessionTicket request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
        => ValueTask.FromResult(new SessionTicket($"ticket-{request.PlayerId}"));
}
// --8<-- [end:clientserver-handler]
