/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import systems.zlink.httpclient.internal.CookieJar;

/** Direct unit tests for the wrapper-owned cookie jar (mirrors C++ {@code cookie_jar.cpp}). */
final class CookieJarTest {

    @Test
    void storesAndMatchesByPathAndHost() {
        CookieJar jar = new CookieJar();
        jar.store("api.test", "sid=abc; Path=/secure");
        jar.store("api.test", "root=1");
        jar.store("other.test", "x=9");

        assertEquals("root=1", jar.headerFor("api.test", "/", false));
        assertTrue(jar.headerFor("api.test", "/secure/data", false).contains("sid=abc"));
        assertFalse(jar.headerFor("api.test", "/", false).contains("sid"));
        assertEquals("x=9", jar.headerFor("other.test", "/", false));
    }

    @Test
    void secureCookieOnlyOnSecureRequests() {
        CookieJar jar = new CookieJar();
        jar.store("api.test", "tok=1; Secure");
        assertEquals("", jar.headerFor("api.test", "/", false));
        assertEquals("tok=1", jar.headerFor("api.test", "/", true));
    }

    @Test
    void maxAgeZeroDeletesAndOverwrites() {
        CookieJar jar = new CookieJar();
        jar.store("api.test", "a=1");
        jar.store("api.test", "a=2");
        assertEquals("a=2", jar.headerFor("api.test", "/", false));
        jar.store("api.test", "a=; Max-Age=0");
        assertEquals("", jar.headerFor("api.test", "/", false));
    }

    @Test
    void evictsOldestBeyondHostLimit() {
        CookieJar jar = new CookieJar();
        for (int i = 0; i < 130; i++) {
            jar.store("api.test", "c" + i + "=v");
        }
        String header = jar.headerFor("api.test", "/", false);
        assertFalse(header.contains("c0="));
        assertFalse(header.contains("c1="));
        assertTrue(header.contains("c129="));
    }

    @Test
    void ignoresMalformedSetCookie() {
        CookieJar jar = new CookieJar();
        jar.store("api.test", "");
        jar.store("api.test", "novalue");
        jar.store("api.test", "=novalue");
        jar.store("api.test", "bad=1; Max-Age=notanumber");
        assertEquals("bad=1", jar.headerFor("api.test", "/", false));
    }
}
