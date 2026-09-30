package systems.zlink.framework.perf.servers.actorcaller;

import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;

public final class Program {
    private Program() {}

    public static void main(String[] args) {
        RoleConfig config = ServerApplication.readConfig(args);
        if (!"actor-caller".equals(config.role()) || !config.source()) {
            throw new IllegalArgumentException("actor-caller-server runs the source role of §10.9 and §10.10.");
        }
        switch (config.scenario()) {
            case "actor-no-bind-request-echo" -> ActorNoBindRequestEchoScenario.run(config);
            case "actor-no-bind-send-send-echo" -> ActorNoBindSendSendEchoScenario.run(config);
            default -> throw new IllegalArgumentException("actor-caller-server does not run scenario '" + config.scenario() + "'.");
        }
    }
}
