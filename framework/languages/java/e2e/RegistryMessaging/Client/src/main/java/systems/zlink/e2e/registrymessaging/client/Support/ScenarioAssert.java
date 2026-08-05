package systems.zlink.e2e.registrymessaging.client.Support;

import java.util.Arrays;
import java.util.Set;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class ScenarioAssert {
    private ScenarioAssert() {
    }

    public static void that(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    public static void expectFailure(Runnable action) {
        try {
            action.run();
        } catch (RuntimeException error) {
            return;
        }
        throw new IllegalStateException("operation unexpectedly succeeded");
    }

    public static String[] evidence(ZLinkHttpClient role) {
        return role.get("/evidence").submit(String[].class).toCompletableFuture().join().body();
    }

    public static String[] waitEvidence(ZLinkHttpClient role, String contains) {
        return role.post("/evidence/wait")
            .body(new Contracts.EvidenceWaitReq(contains))
            .submit(String[].class).toCompletableFuture().join().body();
    }

    public static Contracts.ProfileRes requestProfileEventually(
        ZLinkHttpClient role,
        String marker) {
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(15);
        RuntimeException lastError = null;
        while (System.nanoTime() < deadline) {
            try {
                return role.post("/profile/request")
                    .body(new Contracts.ProfileReq(marker))
                    .submit(Contracts.ProfileRes.class).toCompletableFuture().join().body();
            } catch (RuntimeException error) {
                lastError = error;
                sleep(100);
            }
        }
        throw new IllegalStateException("timed out waiting for profile request " + marker, lastError);
    }

    public static Set<String> requestUntilProvidersSeen(
        ZLinkHttpClient role,
        String markerPrefix,
        Set<String> expectedProviders) {
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(30);
        Set<String> providers = new java.util.HashSet<>();
        int index = 0;
        while (System.nanoTime() < deadline && !providers.containsAll(expectedProviders)) {
            Contracts.ProfileRes reply = requestProfileEventually(role, markerPrefix + "-" + index);
            providers.add(reply.providerRid());
            index++;
            sleep(100);
        }
        return providers;
    }

    public static String[] waitAnyEvidence(
        ZLinkHttpClient first,
        ZLinkHttpClient second,
        String contains) {
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(30);
        String[] combined = concat(evidence(first), evidence(second));
        while (System.nanoTime() < deadline) {
            combined = concat(evidence(first), evidence(second));
            if (Arrays.stream(combined).anyMatch(line -> line.contains(contains))) {
                return combined;
            }
            sleep(100);
        }
        throw new IllegalStateException("timed out waiting for evidence containing " + contains);
    }

    public static String[] concat(String[] first, String[] second) {
        return java.util.stream.Stream.concat(Arrays.stream(first), Arrays.stream(second))
            .toArray(String[]::new);
    }

    private static void sleep(long milliseconds) {
        try {
            Thread.sleep(milliseconds);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted while waiting for evidence", error);
        }
    }
}
