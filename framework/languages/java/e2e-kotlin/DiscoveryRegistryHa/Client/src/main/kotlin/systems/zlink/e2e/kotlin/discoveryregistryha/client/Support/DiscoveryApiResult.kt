package Support

import systems.zlink.e2e.kotlin.discoveryregistryha.client.Support
data class DiscoveryApiRes(
    val operation: String,
    val reg1TopologyCount: Int,
    val reg2TopologyCount: Int,
    val reg3TopologyCount: Int,
    val topologyEvidence: List<String>,
    val scenarioEvidence: List<String> = emptyList(),
)
