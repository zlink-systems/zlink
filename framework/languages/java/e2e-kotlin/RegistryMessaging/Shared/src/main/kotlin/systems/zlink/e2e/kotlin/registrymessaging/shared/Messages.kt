package systems.zlink.e2e.kotlin.registrymessaging.shared

import systems.zlink.framework.handlers.ZLinkPacket

object Contracts {
    const val CHANNEL_HANDLER_GROUP = "registry-messaging-channel-handlers"
    const val ROUTE_HANDLER_GROUP = "registry-messaging-route-handlers"
    const val PROFILE_CHANNEL = "profile"
    const val PROFILE_ROUTE_CHANNEL = "profile.route"
    const val WORKFLOW_CHANNEL = "workflow"
    const val PROFILE_REQUEST_PACKET = "ProfileReq"
    const val PROFILE_COMMAND_PACKET = "ProfileMsg"
    const val PAYLOAD_REQUEST_PACKET = "PayloadReq"
    const val ROUTE_PACKET = "ScenarioRoutePingReq"
    const val WORKFLOW_REQUEST_PACKET = "WorkflowReq"
}

data class ProfileReq(
    var value: String = "",
)

data class ProfileRes(
    var value: String = "",
    var providerRid: String = "",
    var instanceId: String = "",
)

data class ProfileMsg(
    var commandId: String = "",
)

@ZLinkPacket("MissingProfileReq")
data class MissingProfileReq(
    var value: String = "",
)

@ZLinkPacket("MissingProfileMsg")
data class MissingProfileMsg(
    var commandId: String = "",
)

data class EvidenceWaitReq(
    var contains: String = "",
    var timeoutMilliseconds: Int = 10000,
)

data class PayloadReq(
    var marker: String = "",
    var payload: String = "",
)

data class PayloadRes(
    var marker: String = "",
    var length: Int = 0,
    var sha256: String = "",
)

data class WorkflowReq(
    var value: String = "",
)

data class WorkflowRes(
    var value: String = "",
    var providerRid: String = "",
)

data class ScenarioRoutePingReq(
    var value: String = "",
)

data class ScenarioRoutePingRes(
    var value: String = "",
    var providerRid: String = "",
    var sourceRid: String = "",
)

data class RouteMissingRes(
    var failed: Boolean = false,
)

data class RequestFailureRes(
    var failed: Boolean = false,
    var failureType: String = "",
)

data class BackpressureRes(
    var outcome: String = "",
)
