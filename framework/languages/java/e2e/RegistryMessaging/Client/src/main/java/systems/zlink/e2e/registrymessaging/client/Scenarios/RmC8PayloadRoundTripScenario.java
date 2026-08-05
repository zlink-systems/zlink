package systems.zlink.e2e.registrymessaging.client.Scenarios;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmC8PayloadRoundTripScenario {
    private RmC8PayloadRoundTripScenario() {
    }

    public static void run(
        ZLinkHttpClient singleConsumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB) {
        java.util.List<String> markers = new java.util.ArrayList<>();
        int[] sizes = {1, 4096, 256 * 1024, 1024 * 1024};
        for (int size : sizes) {
            String marker = "rm-c8-" + size + "-" + java.util.UUID.randomUUID();
            markers.add(marker);
            String payload = buildPayload(size);
            Contracts.PayloadRes reply = singleConsumer.post("/profile/payload")
                .body(new Contracts.PayloadReq(marker, payload))
                .submit(Contracts.PayloadRes.class).toCompletableFuture().join().body();
            ScenarioAssert.that(reply.marker().equals(marker), "RM-C8 payload marker mismatch for " + size);
            ScenarioAssert.that(reply.length() == payload.length(), "RM-C8 payload length mismatch for " + size);
            ScenarioAssert.that(reply.sha256().equals(sha256(payload)), "RM-C8 payload hash mismatch for " + size);
        }
        Contracts.ProfileRes followUp = singleConsumer.post("/profile/request")
            .body(new Contracts.ProfileReq("rm-c8-after"))
            .submit(Contracts.ProfileRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that("profile:rm-c8-after".equals(followUp.value()), "RM-C8 follow-up request failed");
        String[] evidence = ScenarioAssert.concat(
            ScenarioAssert.evidence(providerA),
            ScenarioAssert.evidence(providerB));
        ScenarioAssert.that(markers.stream().allMatch(marker -> java.util.Arrays.stream(evidence)
                .anyMatch(line -> line.contains("payload-request") && line.contains(marker))),
            "RM-C8 payload evidence missing");
        System.out.println("scenario RM-C8 payload-integrity passed");
    }

    private static String buildPayload(int size) {
        StringBuilder builder = new StringBuilder(size);
        for (int index = 0; index < size; index++) {
            builder.append((char) ('a' + (index % 26)));
        }
        return builder.toString();
    }

    private static String sha256(String payload) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            return HexFormat.of().formatHex(digest.digest(payload.getBytes(StandardCharsets.UTF_8))).toUpperCase();
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException("SHA-256 unavailable", error);
        }
    }
}
