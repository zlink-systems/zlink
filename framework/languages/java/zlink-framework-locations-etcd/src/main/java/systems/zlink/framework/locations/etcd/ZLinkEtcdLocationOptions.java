package systems.zlink.framework.locations.etcd;

import java.time.Duration;
import java.util.List;

public final class ZLinkEtcdLocationOptions {
    private List<String> endpoints = List.of();
    private ZLinkEtcdTlsOptions tls;
    private ZLinkEtcdCredentials credentials;
    private String namespace;
    private Duration operationTimeout = Duration.ofSeconds(5);

    public List<String> endpoints() {
        return endpoints;
    }

    public ZLinkEtcdLocationOptions setEndpoints(List<String> value) {
        endpoints = value;
        return this;
    }

    public ZLinkEtcdTlsOptions tls() {
        return tls;
    }

    public ZLinkEtcdLocationOptions setTls(ZLinkEtcdTlsOptions value) {
        tls = value;
        return this;
    }

    public ZLinkEtcdCredentials credentials() {
        return credentials;
    }

    public ZLinkEtcdLocationOptions setCredentials(ZLinkEtcdCredentials value) {
        credentials = value;
        return this;
    }

    public String namespace() {
        return namespace;
    }

    public ZLinkEtcdLocationOptions setNamespace(String value) {
        namespace = value;
        return this;
    }

    public Duration operationTimeout() {
        return operationTimeout;
    }

    public ZLinkEtcdLocationOptions setOperationTimeout(Duration value) {
        operationTimeout = value;
        return this;
    }
}
