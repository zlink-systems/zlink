package systems.zlink.samples.kotlin.supportchat.server.api.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.configuration.SupportChatRoles
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AuthenticateUserReq
import systems.zlink.samples.kotlin.supportchat.shared.contracts.AuthenticateUserRes

@ZLinkHandlerGroup(SampleNames.ApiChannel)
class AuthenticateUserHandler : ZLinkSuspendingRequestHandler<AuthenticateUserReq, AuthenticateUserRes> {
    override suspend fun handle(
        request: AuthenticateUserReq,
        context: ZLinkMessageContext,
    ): AuthenticateUserRes =
        when (request.accessToken) {
            "customer-1" -> accepted("customer-1", "Customer 1", SupportChatRoles.Customer)
            "customer-2" -> accepted("customer-2", "Customer 2", SupportChatRoles.Customer)
            "customer-3" -> accepted("customer-3", "Customer 3", SupportChatRoles.Customer)
            "agent-1" -> accepted("agent-1", "Agent 1", SupportChatRoles.Agent)
            "agent-2" -> accepted("agent-2", "Agent 2", SupportChatRoles.Agent)
            else -> AuthenticateUserRes(false, null, null, null, "Unknown support chat token.")
        }

    private fun accepted(
        actorId: String,
        displayName: String,
        role: String,
    ): AuthenticateUserRes = AuthenticateUserRes(true, actorId, displayName, role, null)
}
