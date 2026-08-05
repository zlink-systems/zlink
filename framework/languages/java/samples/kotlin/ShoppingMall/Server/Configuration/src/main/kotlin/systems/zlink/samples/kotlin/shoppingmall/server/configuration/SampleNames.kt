package systems.zlink.samples.kotlin.shoppingmall.server.configuration

object SampleNames {
    const val CommerceApiChannel = "shoppingmall.commerce.api"
    const val OrderWorkflowMesh = "shoppingmall.orders"
    const val OrderWorkflowSpotType = "shoppingmall.order-workflow"

    const val ApiInstanceA = "api-a"
    const val ApiInstanceB = "api-b"
    const val WorkflowInstanceA = "workflow-a"
    const val WorkflowInstanceB = "workflow-b"

    fun commerceApiChannel(instanceId: String): String = "$CommerceApiChannel.$instanceId"
}
