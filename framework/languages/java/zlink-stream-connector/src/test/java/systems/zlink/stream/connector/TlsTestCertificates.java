package systems.zlink.stream.connector;

import java.io.File;
import java.net.URISyntaxException;
import java.net.URL;

final class TlsTestCertificates {
    private TlsTestCertificates() {
    }

    static File certificate() {
        return resourceFile("tls-test-cert.pem");
    }

    static File privateKey() {
        return resourceFile("tls-test-key.pem");
    }

    private static File resourceFile(String name) {
        URL resource = TlsTestCertificates.class.getClassLoader().getResource(name);
        if (resource == null) {
            throw new IllegalStateException("missing test TLS resource: " + name);
        }
        try {
            return new File(resource.toURI());
        } catch (URISyntaxException ex) {
            throw new IllegalStateException("invalid test TLS resource URI: " + name, ex);
        }
    }
}
