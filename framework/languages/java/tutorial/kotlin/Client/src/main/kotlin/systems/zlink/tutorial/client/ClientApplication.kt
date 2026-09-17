package systems.zlink.tutorial.client

import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.runApplication
import org.springframework.context.annotation.Bean
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication
class ClientApplication {

    @Bean
    fun zlink(): ZLinkFrameworkConfigurer = ZLinkFrameworkConfigurer { options ->
        // --8<-- [start:location-store-client]
        // Rooms are looked up by whoever calls them, so a node that hosts none
        // still needs the store, pointed at the same prefix. Every other peer
        // this node needs is named by hand below.
        options.addLocationStore(
            ZLinkRedisLocationStore(
                ZLinkRedisLocationOptions()
                    .setConnectionString("127.0.0.1:6379")
                    .setKeyPrefix("zlink-tutorial-kotlin:location:"),
            ),
        )
        // --8<-- [end:location-store-client]

        // --8<-- [start:channel-client-register]
        // This node opens an endpoint too. Both sides listen to become peers.
        val mesh = options.addRouteMesh("game").listen("tcp://0.0.0.0:7602")

        // client() means this node exposes no handler for the channel; it only calls.
        mesh.channelName("profile").client()

        // A mesh peer connection, not a channel one. The mesh picks a node that
        // serves the channel from among the peers it learns this way, so a channel
        // call never names a node.
        //
        // The routing id form states which node is expected at that endpoint. The
        // peer is rejected unless it answers with that id AND advertises exactly
        // this endpoint string -- which is why the server sets its advertise host.
        // connect(endpoint) alone would connect to whoever is there.
        mesh.peerConnections().connect(
            RoutingId.from("game-server-1"),
            "tcp://127.0.0.1:7601",
        )
        // --8<-- [end:channel-client-register]

        // --8<-- [start:clientserver-client-register]
        // Here the caller decides who answers: the server it dialed. Mesh peers
        // play no part in the choice.
        options.addClientServerChannel("ticketing")
            .client()
            .connect("tcp://127.0.0.1:7611")
        // --8<-- [end:clientserver-client-register]

        // --8<-- [start:fanout-publish-register]
        // The publisher keeps no subscriber list. Subscribers may come and go with
        // no change here.
        options.addFanoutChannel("broadcast")
            .setRoutingIdPrefix("game-client-broadcast")
            .enablePublisher("tcp://127.0.0.1:7612")
        // --8<-- [end:fanout-publish-register]

        // --8<-- [start:spot-client-register]
        // client() rules out registering factories. This node creates and calls
        // rooms; a node that picked server() runs them.
        mesh.objects().client()
        // --8<-- [end:spot-client-register]
    }
}

fun main(args: Array<String>) {
    runApplication<ClientApplication>(*args)
}
