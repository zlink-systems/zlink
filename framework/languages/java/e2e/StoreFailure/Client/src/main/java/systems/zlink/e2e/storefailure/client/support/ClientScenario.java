package systems.zlink.e2e.storefailure.client.support;

public interface ClientScenario {
    DiscoveryApiResult run(ClientContext context) throws Exception;
}
