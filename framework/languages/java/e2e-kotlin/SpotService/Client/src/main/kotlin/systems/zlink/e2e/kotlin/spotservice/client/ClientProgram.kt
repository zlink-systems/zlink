package systems.zlink.e2e.kotlin.spotservice.client

import systems.zlink.e2e.kotlin.spotservice.Env
import kotlinx.coroutines.runBlocking

fun main(args: Array<String>) {
    Env.configure(args)
    val mode = Env.get("e2e.client.mode", "all")
    runBlocking { ClientScenario().runMode(mode) }
    println("spot-service kotlin e2e mode=$mode result=passed")
}
