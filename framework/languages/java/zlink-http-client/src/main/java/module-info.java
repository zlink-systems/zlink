module zlink.http.client {
    requires transitive systems.zlink.framework;
    requires transitive com.fasterxml.jackson.databind;
    requires java.net.http;
    requires java.logging;

    exports systems.zlink.httpclient;
}
