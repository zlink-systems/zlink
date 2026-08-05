using SupportChat.Server.Configuration;
using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;

namespace SupportChat.Server.Api.Handlers;

[ZLinkHandlerGroup("api")]
internal sealed class AuthenticateUserHandler : IZLinkRequestHandler<AuthenticateUserReq, AuthenticateUserRes>
{
    public ValueTask<AuthenticateUserRes> HandleAsync(
        AuthenticateUserReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        return ValueTask.FromResult(request.AccessToken switch
        {
            "customer-1" => Accepted("customer-1", "Customer 1", SupportChatRoles.Customer),
            "customer-2" => Accepted("customer-2", "Customer 2", SupportChatRoles.Customer),
            "customer-3" => Accepted("customer-3", "Customer 3", SupportChatRoles.Customer),
            "agent-1" => Accepted("agent-1", "Agent 1", SupportChatRoles.Agent),
            "agent-2" => Accepted("agent-2", "Agent 2", SupportChatRoles.Agent),
            _ => new AuthenticateUserRes(false, null, null, null, "Unknown support chat token.")
        });
    }

    private static AuthenticateUserRes Accepted(
        string actorId,
        string displayName,
        string role)
    {
        return new AuthenticateUserRes(true, actorId, displayName, role, null);
    }
}
