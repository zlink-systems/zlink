package systems.zlink.framework.locations.etcd;

public final class ZLinkEtcdTlsOptions {
    private String trustedCaPem;
    private ZLinkEtcdClientIdentity clientIdentity;

    public String trustedCaPem() {
        return trustedCaPem;
    }

    public ZLinkEtcdTlsOptions setTrustedCaPem(String value) {
        trustedCaPem = value;
        return this;
    }

    public ZLinkEtcdClientIdentity clientIdentity() {
        return clientIdentity;
    }

    public ZLinkEtcdTlsOptions setClientIdentity(ZLinkEtcdClientIdentity value) {
        clientIdentity = value;
        return this;
    }
}
