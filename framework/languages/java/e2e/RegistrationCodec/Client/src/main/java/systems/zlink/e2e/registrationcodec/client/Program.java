package systems.zlink.e2e.registrationcodec.client;

import systems.zlink.e2e.registrationcodec.client.Support.ClientOptions;
import systems.zlink.e2e.registrationcodec.client.Support.Evidence;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        ClientOptions options = ClientOptions.load(args);
        try (
            ZLinkHttpClient server = ZLinkHttpClient.create(options.httpEndpoint()).build();
            ZLinkHttpClient codecRequester = ZLinkHttpClient.create(options.codecRequesterHttpEndpoint()).build();
            Evidence evidence = Evidence.fromOptions(options)
        ) {
            new ScenarioRunner(new ScenarioContext(options, server, codecRequester, evidence)).run();
            System.out.println("registration-codec e2e result=passed");
        }
    }
}
