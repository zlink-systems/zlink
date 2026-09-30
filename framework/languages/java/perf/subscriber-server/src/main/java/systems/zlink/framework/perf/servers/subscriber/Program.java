package systems.zlink.framework.perf.servers.subscriber;

import systems.zlink.framework.perf.CellDirectory;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfPublishEvent;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;

public final class Program {
    private Program() {}

    public static void main(String[] args) {
        RoleConfig config = ServerApplication.readConfig(args);
        if (!"pubsub-fanout-echo".equals(config.scenario()) || !"subscriber".equals(config.role()) || config.source()) {
            throw new IllegalArgumentException("subscriber-server runs the subscriber role of pubsub-fanout-echo.");
        }
        // Automatic Classic fanout: no endpoint and no subscribe(topic), so this subscriber receives every topic (§10.11).
        ServerApplication app = ServerApplication.create(config)
                .configure(options -> options.addFanoutChannel(config.channelName())
                        .enableSubscriber().addPublishHandler(PerfFanoutHandler.class, PerfPublishEvent.class))
                .bean(ObjectsReadiness.class, () -> new ObjectsReadiness(false, "No Ready publisher is visible to this Subscriber yet."))
                .bean(CellDirectory.class, () -> new CellDirectory(ServerApplication.cellDirectory(args)))
                .bean(FanoutReceipts.class);
        app.start().getBean(FanoutReceipts.class).prepare();
    }
}
