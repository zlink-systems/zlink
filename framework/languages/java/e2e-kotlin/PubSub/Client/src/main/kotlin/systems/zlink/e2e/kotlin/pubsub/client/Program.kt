package systems.zlink.e2e.kotlin.pubsub.client

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.registerKotlinModule
import systems.zlink.e2e.kotlin.pubsub.client.Support.ClientOptions
import systems.zlink.e2e.kotlin.pubsub.client.Support.ScenarioContext

fun main(args: Array<String>) {
    val json = ObjectMapper().registerKotlinModule()
    val context = ScenarioContext(ClientOptions.parse(args), json)
    context.run()
    println("pub-sub kotlin e2e result=passed")
}
