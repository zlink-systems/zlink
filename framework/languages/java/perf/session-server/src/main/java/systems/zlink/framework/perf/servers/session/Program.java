package systems.zlink.framework.perf.servers.session;

import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfActorRelaySession;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.SessionActorSetup;

public final class Program {
    private Program() {}

    public static void main(String[] args) {
        RoleConfig config = ServerApplication.readConfig(args);
        if (!"session".equals(config.role()) || config.source()
                || !("session-echo-only".equals(config.scenario()) || "cs-remote-session-actor-echo".equals(config.scenario()))) {
            throw new IllegalArgumentException("session-server supports the session receiver roles of §10.2 and §11.1.");
        }
        ServerApplication app = ServerApplication.create(config);
        if ("session-echo-only".equals(config.scenario())) {
            app.configure(options -> options.addStreamNode("perf-session")
                    .bind(config.transportEndpoints().get("stream"))
                    .registerSession(PerfSession.class)
                    .addSessionPacketHandler(SessionEchoHandler.class));
        } else {
            // §10.2: an Object Client node; the Actors live in the separate Actor process.
            app.configure(options -> {
                ServerApplication.routeMesh(options, config, "perf-session").objects().client();
                options.addStreamNode("perf-session")
                        .bind(config.transportEndpoints().get("stream"))
                        .enableActorDispatch()
                        .registerSession(PerfActorRelaySession.class);
            });
            app.bean(ObjectsReadiness.class, () -> new ObjectsReadiness(true, ""));
            app.bean(SessionActorSetup.class);
        }
        app.start();
    }
}
