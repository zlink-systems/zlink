package systems.zlink.framework.perf.servers.actor;

import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.perf.ActorPlacementWatcher;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfActorType;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;

public final class Program {
    private Program() {}

    public static void main(String[] args) {
        RoleConfig config = ServerApplication.readConfig(args);
        if (!"actor".equals(config.role()) || !("cs-remote-session-actor-echo".equals(config.scenario())
                || "actor-no-bind-request-echo".equals(config.scenario())
                || "actor-no-bind-send-send-echo".equals(config.scenario()))) {
            throw new IllegalArgumentException("actor-server runs the actor role of §10.2, §10.9 and §10.10.");
        }
        // The Actor Object Server hosts the Actors that a remote Session binds or an ActorCaller addresses by ActorId.
        ServerApplication app = ServerApplication.create(config).configure(options -> {
            ZLinkMeshNodeBuilder mesh = ServerApplication.routeMesh(options, config, "perf-actor");
            // §10.10: the Actor answers through the public Channel client; the caller is the return channel Server.
            if ("send-send".equals(config.mode())) {
                mesh.channelName(config.channelName()).client();
            }
            PerfActorType.addPerfActors(mesh.objects().server());
        });
        app.bean(ObjectsReadiness.class, () -> new ObjectsReadiness(false, "No typed probe has reached an Actor yet."));
        app.bean(ActorPlacementWatcher.class);
        app.start().getBean(ActorPlacementWatcher.class).start();
    }
}
