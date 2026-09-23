package systems.zlink.quickstart.client;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.RestController;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.quickstart.shared.Greeting;
import systems.zlink.quickstart.shared.Hello;

@EnableZLinkFramework
@SpringBootApplication
public class ClientApplication {

    public static void main(String[] args) {
        SpringApplication.run(ClientApplication.class, args);
    }

    @Bean
    ZLinkFrameworkConfigurer zlink() {
        return options -> {
            // This process also needs its own endpoint.
            ZLinkMeshNodeBuilder mesh =
                    options.addRouteMesh("services").listen("tcp://127.0.0.1:7102");
            // This side only calls; it does not handle "greeting".
            mesh.channelName("greeting").client();
            // Manual connection -- the server's endpoint is given directly.
            mesh.peerConnections().connect("tcp://127.0.0.1:7101");
        };
    }
}

@RestController
class HelloController {
    private final ZLinkRouteClient route;

    HelloController(ZLinkRouteClient route) {
        this.route = route;
    }

    @GetMapping("/hello/{name}")
    String hello(@PathVariable String name) {
        // The target is a single ChannelName; which node handles it is not specified.
        // submit_sync blocks this application thread until the reply arrives. A request
        // handler runs on an application thread, not a runtime execution context, so
        // blocking here is allowed.
        return route.requestToChannel("greeting", new Hello(name))
                .submit_sync(Greeting.class)
                .text();
    }
}
