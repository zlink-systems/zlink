package systems.zlink.quickstart.server

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import org.springframework.boot.SpringApplication
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.context.annotation.Bean
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkRequestHandler
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.quickstart.shared.Greeting
import systems.zlink.quickstart.shared.Hello

@EnableZLinkFramework
@SpringBootApplication
class ServerApplication {

    @Bean
    fun zlink(): ZLinkFrameworkConfigurer = ZLinkFrameworkConfigurer { options ->
        // Discovers handler types.
        options.addHandlersFromPackageOf(ServerApplication::class.java)

        // Names the mesh.
        val mesh = options.addRouteMesh("services")
            // This process's own endpoint, for peers to connect to.
            .listen("tcp://0.0.0.0:7101")
        // This process handles the "greeting" channel.
        mesh.channelName("greeting").server()
            .addRequestHandler(HelloHandler::class.java, Hello::class.java, Greeting::class.java)
    }
}

fun main(args: Array<String>) {
    // This process serves no HTTP; without setKeepAlive the JVM exits as soon
    // as the context finishes refreshing.
    val app = SpringApplication(ServerApplication::class.java)
    app.isKeepAlive = true
    app.run(*args)
}

// Handles one request on the "greeting" channel. ZLinkRequestHandler returns a
// CompletionStage; a Kotlin suspend fun cannot override it. See ../../../README.md.
class HelloHandler : ZLinkRequestHandler<Hello, Greeting> {

    override fun handle(request: Hello, context: ZLinkMessageContext): CompletionStage<Greeting> =
        CompletableFuture.completedFuture(Greeting("hello, ${request.name}"))
}
