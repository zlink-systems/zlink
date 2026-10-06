package systems.zlink.framework.perf.servers.sessionactorlocal;

import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfActorRelaySession;
import systems.zlink.framework.perf.PerfActorType;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.SessionActorSetup;

public final class Program {
    private Program() {}

    public static void main(String[] args) {
        RoleConfig config = ServerApplication.readConfig(args);
        if (!"cs-local-session-actor-echo".equals(config.scenario())
                || !"session-actor-local".equals(config.role())) {
            throw new IllegalArgumentException(
                    "session-actor-local-server runs the cs-local-session-actor-echo role.");
        }
        // §10.1: the STREAM session and the Actors it binds live on this one Object Server node.
        ServerApplication app =
                ServerApplication.create(config)
                        .configure(
                                options -> {
                                    PerfActorType.addPerfActors(
                                            ServerApplication.routeMesh(
                                                            options,
                                                            config,
                                                            "perf-session-actor-local")
                                                    .objects()
                                                    .server());
                                    options.addStreamNode("perf-session")
                                            .bind(config.transportEndpoints().get("stream"))
                                            .enableActorDispatch()
                                            .registerSession(PerfActorRelaySession.class);
                                });
        app.bean(
                ObjectsReadiness.class,
                () -> new ObjectsReadiness(false, "No Actor is bound to a Session yet."));
        app.bean(SessionActorSetup.class);
        app.start();
    }
}
