package systems.zlink.e2e.registrationcodec.client.Support;

import systems.zlink.httpclient.ZLinkHttpClient;

public record ScenarioContext(
    ClientOptions options,
    ZLinkHttpClient server,
    ZLinkHttpClient codecRequester,
    Evidence evidence) {
}
