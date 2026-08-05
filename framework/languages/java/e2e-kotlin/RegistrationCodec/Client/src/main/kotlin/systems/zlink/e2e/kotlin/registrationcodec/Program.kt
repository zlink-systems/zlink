package systems.zlink.e2e.kotlin.registrationcodec

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.registerKotlinModule
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ClientOptions

fun main(args: Array<String>) {
    Env.configure(args)
    val json = ObjectMapper().registerKotlinModule()
    ClientScenario(json, ClientOptions.parse(args)).run()
    println("registration-codec kotlin e2e result=passed")
}
