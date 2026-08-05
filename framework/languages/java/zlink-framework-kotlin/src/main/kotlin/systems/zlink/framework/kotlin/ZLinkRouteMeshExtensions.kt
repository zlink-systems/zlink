package systems.zlink.framework.kotlin

import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.configuration.ZLinkFrameworkOptions
import systems.zlink.framework.configuration.ZLinkMeshChannelBuilder
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder
import systems.zlink.framework.configuration.ZLinkMeshPeerConnections

fun ZLinkFrameworkOptions.routeMesh(
    meshName: String,
    configure: ZLinkMeshNodeBuilder.() -> Unit,
): ZLinkMeshNodeBuilder =
    addRouteMesh(meshName).also(configure)

fun ZLinkMeshNodeBuilder.channelName(
    channelName: String,
    configure: ZLinkMeshChannelBuilder.() -> Unit = {},
): ZLinkMeshChannelBuilder =
    channelName(channelName).also(configure)

fun ZLinkMeshPeerConnections.connect(
    expectedRoutingId: RoutingId,
    endpoint: String,
) {
    connect(expectedRoutingId, endpoint)
}
