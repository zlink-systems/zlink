package systems.zlink.e2e.registrymessaging.client;

import systems.zlink.e2e.registrymessaging.client.Support.RegistryMessagingHttp;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioCatalog;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        Inputs inputs = parseInputs(args);
        var options = systems.zlink.e2e.registrymessaging.client.Support.ClientOptions.load(inputs.configPath());
        try (RegistryMessagingHttp http = new RegistryMessagingHttp(options)) {
            new ScenarioCatalog(http, options).run(inputs.scenario());
        }
        System.out.println("registry-messaging e2e result=passed");
    }

    private static Inputs parseInputs(String[] args) {
        if (args.length != 4 || !"--config".equals(args[0]) || args[1].isBlank()
            || !"--scenario".equals(args[2]) || args[3].isBlank()) {
            throw new IllegalArgumentException(
                "Usage: registry-messaging-client --config <path> --scenario <selector>");
        }
        return new Inputs(args[1], args[3]);
    }

    private record Inputs(String configPath, String scenario) { }
}
