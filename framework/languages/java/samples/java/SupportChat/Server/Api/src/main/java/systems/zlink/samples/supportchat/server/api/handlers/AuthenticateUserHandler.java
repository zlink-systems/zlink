package systems.zlink.samples.supportchat.server.api.handlers;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

@ZLinkHandlerGroup(SampleNames.ApiChannel)
public final class AuthenticateUserHandler
    implements ZLinkRequestHandler<Messages.AuthenticateUserReq, Messages.AuthenticateUserRes> {
    @Override
    public java.util.concurrent.CompletionStage<Messages.AuthenticateUserRes> handle(
        Messages.AuthenticateUserReq request,
        ZLinkMessageContext context) {
        return java.util.concurrent.CompletableFuture.completedFuture(switch (request.accessToken()) {
            case "customer-1" -> accepted("customer-1", "Customer 1", SampleNames.Roles.Customer);
            case "customer-2" -> accepted("customer-2", "Customer 2", SampleNames.Roles.Customer);
            case "customer-3" -> accepted("customer-3", "Customer 3", SampleNames.Roles.Customer);
            case "agent-1" -> accepted("agent-1", "Agent 1", SampleNames.Roles.Agent);
            case "agent-2" -> accepted("agent-2", "Agent 2", SampleNames.Roles.Agent);
            default -> new Messages.AuthenticateUserRes(false, null, null, null, "invalid token");
        });
    }

    private static Messages.AuthenticateUserRes accepted(
        String actorId,
        String displayName,
        String role) {
        return new Messages.AuthenticateUserRes(true, actorId, displayName, role, null);
    }
}
