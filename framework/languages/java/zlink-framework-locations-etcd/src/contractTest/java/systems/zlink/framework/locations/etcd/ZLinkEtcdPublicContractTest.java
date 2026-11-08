package systems.zlink.framework.locations.etcd;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;

import java.lang.reflect.Modifier;
import java.time.Duration;
import java.util.Arrays;
import java.util.Set;
import java.util.stream.Collectors;

class ZLinkEtcdPublicContractTest {
    @Test
    void extensionDeclaresOnlyTheApprovedPublicMembers() {
        assertEquals(
                Set.of("read", "write", "scan", "close"),
                publicMethods(ZLinkEtcdLocationStore.class));
        assertEquals(
                Set.of(
                        "endpoints",
                        "setEndpoints",
                        "tls",
                        "setTls",
                        "credentials",
                        "setCredentials",
                        "namespace",
                        "setNamespace",
                        "operationTimeout",
                        "setOperationTimeout"),
                publicMethods(ZLinkEtcdLocationOptions.class));
        assertEquals(
                Set.of("trustedCaPem", "setTrustedCaPem", "clientIdentity", "setClientIdentity"),
                publicMethods(ZLinkEtcdTlsOptions.class));
        assertEquals(
                Set.of("certificatePem", "keyPem"), publicMethods(ZLinkEtcdClientIdentity.class));
        assertEquals(Set.of("user", "password"), publicMethods(ZLinkEtcdCredentials.class));
        assertTrue(ZLinkLocationStore.class.isAssignableFrom(ZLinkEtcdLocationStore.class));
        assertTrue(AutoCloseable.class.isAssignableFrom(ZLinkEtcdLocationStore.class));
        assertEquals(1, ZLinkEtcdLocationStore.class.getConstructors().length);
        assertEquals(Duration.ofSeconds(5), new ZLinkEtcdLocationOptions().operationTimeout());
        for (var method : ZLinkEtcdLocationStore.class.getMethods()) {
            assertFalse(method.getGenericReturnType().getTypeName().contains("internal.proto"));
            for (var parameter : method.getGenericParameterTypes())
                assertFalse(parameter.getTypeName().contains("internal.proto"));
        }
    }

    private static Set<String> publicMethods(Class<?> type) {
        return Arrays.stream(type.getDeclaredMethods())
                .filter(method -> Modifier.isPublic(method.getModifiers()))
                .map(method -> method.getName())
                .collect(Collectors.toSet());
    }
}
