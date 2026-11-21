package systems.zlink.tutorial.client

import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.RestController
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToNode
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
import systems.zlink.tutorial.shared.GetNodeStatus
import systems.zlink.tutorial.shared.NodeStatus

@RestController
class OpsEndpoints(route: ZLinkRouteClient, private val runtime: ZLinkFrameworkRuntime) {

    private val route = route.kotlin()

    @GetMapping("/ops/ready")
    fun readiness(): ResponseEntity<Void> =
        if (runtime.status().isReady()) ResponseEntity.ok().build()
        else ResponseEntity.status(503).build()

    // --8<-- [start:node-direct-call]
    @GetMapping("/ops/nodes/{nodeRid}/status")
    suspend fun nodeStatus(@PathVariable nodeRid: String): NodeStatus =
        // The target is one node, named by its routing id. No channel takes part,
        // so no candidate is chosen: this node answers or the call fails.
        route.requestToNode<NodeStatus>("game", RoutingId.from(nodeRid), GetNodeStatus()).await()
    // --8<-- [end:node-direct-call]
}
