package systems.zlink.quickstart.client

import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.runApplication
import org.springframework.context.annotation.Bean
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.RestController
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToChannel
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.quickstart.shared.Greeting
import systems.zlink.quickstart.shared.Hello

@EnableZLinkFramework
@SpringBootApplication
class ClientApplication {

    @Bean
    fun zlink(): ZLinkFrameworkConfigurer = ZLinkFrameworkConfigurer { options ->
        // This process also needs its own endpoint.
        val mesh = options.addRouteMesh("services").listen("tcp://0.0.0.0:7102")
        // This side only calls; it does not handle "greeting".
        mesh.channelName("greeting").client()
        // Manual connection -- the server's endpoint is given directly.
        mesh.peerConnections().connect("tcp://127.0.0.1:7101")
    }
}

fun main(args: Array<String>) {
    runApplication<ClientApplication>(*args)
}

@RestController
class HelloController(routeClient: ZLinkRouteClient) {

    private val route = routeClient.kotlin()

    @GetMapping("/hello/{name}")
    suspend fun hello(@PathVariable name: String): String =
        // The target is a single ChannelName; which node handles it is not specified.
        route.requestToChannel<Greeting>("greeting", Hello(name)).await().text
}
