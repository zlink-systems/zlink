module zlink.http.client {
    requires transitive systems.zlink.framework;
    requires transitive com.fasterxml.jackson.databind;
    requires java.net.http;

    exports systems.zlink.httpclient;
}
