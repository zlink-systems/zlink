package systems.zlink.framework.locations.etcd;

public final class ZLinkEtcdCredentials {
    private final String user;
    private final String password;

    public ZLinkEtcdCredentials(String user, String password) {
        this.user = user;
        this.password = password;
    }

    public String user() {
        return user;
    }

    public String password() {
        return password;
    }
}
