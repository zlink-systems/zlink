package systems.zlink.framework.runtime.internal.transport;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;

import org.junit.jupiter.api.Test;

/**
 * Pins {@code doc/plan/endpoint-notation-policy.ko.md} behavior for the Java
 * normalization utility, mirroring the C++ reference implementation's test
 * intent (framework/languages/cpp/framework/src/runtime/transport/
 * endpoint_notation.hpp, commit dfcb2177c9).
 */
final class ZLinkEndpointNotationTest {

    @Test
    void schemeAndHostAreLowercased() {
        assertEquals(
            "tcp://example.com:80",
            ZLinkEndpointNotation.normalize("TCP://Example.COM:80"));
    }

    @Test
    void leadingZerosAreStrippedFromThePort() {
        assertEquals(
            "tcp://host:80",
            ZLinkEndpointNotation.normalize("tcp://host:0080"));
        assertEquals(
            "tcp://host:0",
            ZLinkEndpointNotation.normalize("tcp://host:0000"));
    }

    @Test
    void trailingSlashIsStrippedFromThePath() {
        assertEquals(
            "tcp://host:80/path",
            ZLinkEndpointNotation.normalize("tcp://host:80/path///"));
    }

    @Test
    void surroundingWhitespaceIsTrimmed() {
        assertEquals(
            "tcp://host:80",
            ZLinkEndpointNotation.normalize("  tcp://host:80  "));
    }

    @Test
    void ipv6IsBracketedAndZoneIdCasePreserved() {
        // Already bracketed, uppercase IP + uppercase zone id: IP lowered,
        // zone id preserved verbatim (including case).
        assertEquals(
            "tcp://[fe80::1%Eth0]:80",
            ZLinkEndpointNotation.normalize("tcp://[FE80::1%Eth0]:80"));
    }

    @Test
    void unbracketedIpv6IsBracketedWithoutGuessingAPort() {
        // 2+ colons, no brackets: bracket the whole thing, do not attempt
        // to split off a port (that would be exactly the lastIndexOf(':')
        // mistake §2.4 forbids).
        assertEquals(
            "tcp://[2001:db8::1]",
            ZLinkEndpointNotation.normalize("tcp://2001:DB8::1"));
    }

    @Test
    void losslessRoundTripPreservesUserInfoQueryAndFragment() {
        assertEquals(
            "tcp://user:pass@host:80/path?Query=Val&x=1#Frag",
            ZLinkEndpointNotation.normalize(
                "TCP://user:pass@HOST:0080/path?Query=Val&x=1#Frag"));
    }

    @Test
    void normalizeIsIdempotent() {
        String once = ZLinkEndpointNotation.normalize(
            "TCP://[FE80::1%Eth0]:0080/path/?q=1#f");
        String twice = ZLinkEndpointNotation.normalize(once);
        assertEquals(once, twice);
    }

    @Test
    void localhostAndLoopbackAreDifferentEndpoints() {
        assertNotEquals(
            ZLinkEndpointNotation.normalize("tcp://localhost:80"),
            ZLinkEndpointNotation.normalize("tcp://127.0.0.1:80"));
    }

    @Test
    void nonAuthoritySchemeOnlyLowercasesTheScheme() {
        // ipc:// is a filesystem-path scheme: only the scheme casing
        // changes, the path bytes -- including case and trailing slash --
        // are preserved exactly.
        assertEquals(
            "ipc:///tmp/Foo/Bar/",
            ZLinkEndpointNotation.normalize("IPC:///tmp/Foo/Bar/"));
    }

    @Test
    void uppercaseSchemeIsAccepted() {
        assertEquals(
            "tcp://host:80",
            ZLinkEndpointNotation.normalize("TCP://host:80"));
        assertEquals(
            "wss://host:443",
            ZLinkEndpointNotation.normalize("WSS://host:443"));
    }

    @Test
    void malformedBracketRefusesToGuessAndReturnsTrimmedOriginal() {
        assertEquals(
            "tcp://[2001:db8::1",
            ZLinkEndpointNotation.normalize("  tcp://[2001:db8::1  "));
    }

    @Test
    void singleColonWithNonDigitPortRefusesToGuess() {
        assertEquals(
            "tcp://host:notaport",
            ZLinkEndpointNotation.normalize("tcp://host:notaport"));
    }

    @Test
    void noSchemeIsOnlyTrimmed() {
        assertEquals("host:80", ZLinkEndpointNotation.normalize("  host:80  "));
    }

    @Test
    void nullIsPassedThrough() {
        assertNull(ZLinkEndpointNotation.normalize(null));
    }

    @Test
    void emptyAndBlankAreTrimmedToEmpty() {
        assertEquals("", ZLinkEndpointNotation.normalize(""));
        assertEquals("", ZLinkEndpointNotation.normalize("   "));
    }

    @Test
    void bracketIpv6HostWrapsOnlyBareIpv6Shapes() {
        assertEquals("[2001:db8::1]", ZLinkEndpointNotation.bracketIpv6Host("2001:db8::1"));
        assertEquals("[already]", ZLinkEndpointNotation.bracketIpv6Host("[already]"));
        assertEquals("plainhost", ZLinkEndpointNotation.bracketIpv6Host("plainhost"));
    }

    @Test
    void withHostReplacesHostIpv6SafelyAndNormalizes() {
        assertEquals(
            "tcp://[2001:db8::2]:80",
            ZLinkEndpointNotation.withHost("tcp://old-host:80", "2001:db8::2"));
        assertEquals(
            "tcp://newhost:80/path",
            ZLinkEndpointNotation.withHost("TCP://[2001:db8::1]:80/path", "NewHost"));
    }

    @Test
    void withHostRefusesToGuessOnUnparseableEndpoint() {
        String malformed = "tcp://[2001:db8::1";
        assertEquals(malformed, ZLinkEndpointNotation.withHost(malformed, "host"));
    }

    @Test
    void withHostLeavesEndpointUnchangedWhenNewHostBlank() {
        assertEquals(
            "tcp://host:80",
            ZLinkEndpointNotation.withHost("tcp://host:80", ""));
        assertEquals(
            "tcp://host:80",
            ZLinkEndpointNotation.withHost("tcp://host:80", null));
    }
}
