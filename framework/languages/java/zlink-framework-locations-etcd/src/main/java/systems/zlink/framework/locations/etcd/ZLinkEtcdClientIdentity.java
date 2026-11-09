package systems.zlink.framework.locations.etcd;

public final class ZLinkEtcdClientIdentity {
    private final String certificatePem;
    private final String keyPem;

    public ZLinkEtcdClientIdentity(String certificatePem, String keyPem) {
        this.certificatePem = certificatePem;
        this.keyPem = keyPem;
    }

    public String certificatePem() {
        return certificatePem;
    }

    public String keyPem() {
        return keyPem;
    }
}
