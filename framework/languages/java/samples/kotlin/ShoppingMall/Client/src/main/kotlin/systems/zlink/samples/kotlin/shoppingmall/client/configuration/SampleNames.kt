package systems.zlink.samples.kotlin.shoppingmall.client.configuration

object SampleNames {
    const val CommerceApiChannel = "shoppingmall.commerce.api"
    const val ApiInstanceA = "api-a"
    const val ApiInstanceB = "api-b"

    fun commerceApiChannel(instanceId: String): String = "$CommerceApiChannel.$instanceId"
}
