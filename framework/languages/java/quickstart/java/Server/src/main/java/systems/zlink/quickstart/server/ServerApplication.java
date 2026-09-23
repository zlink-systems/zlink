package systems.zlink.quickstart.server;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.quickstart.shared.Greeting;
import systems.zlink.quickstart.shared.Hello;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

@EnableZLinkFramework
@SpringBootApplication
public class ServerApplication {

    public static void main(String[] args) {
        // This process serves no HTTP; without setKeepAlive the JVM exits as
        // soon as the context finishes refreshing.
        SpringApplication app = new SpringApplication(ServerApplication.class);
        app.setKeepAlive(true);
        app.run(args);
    }

    @Bean
    ZLinkFrameworkConfigurer zlink() {
        return options -> {
            // Discovers handler types.
            options.addHandlersFromPackageOf(ServerApplication.class);

            // Names the mesh.
            ZLinkMeshNodeBuilder mesh =
                    options.addRouteMesh("services")
                            // This process's own endpoint, for peers to connect to.
                            .listen("tcp://127.0.0.1:7101");
            // This process handles the "greeting" channel.
            mesh.channelName("greeting")
                    .server()
                    .addRequestHandler(HelloHandler.class, Hello.class, Greeting.class);
        };
    }
}

// Handles one request on the "greeting" channel.
final class HelloHandler implements ZLinkRequestHandler<Hello, Greeting> {

    @Override
    public CompletionStage<Greeting> handle(Hello request, ZLinkMessageContext context) {
        return CompletableFuture.completedFuture(new Greeting("hello, " + request.name()));
    }
}
