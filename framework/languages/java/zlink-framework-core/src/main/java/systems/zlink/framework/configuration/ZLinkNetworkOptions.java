package systems.zlink.framework.configuration;

import java.util.Optional;

/** Process-wide defaults used by listener builders. */
public interface ZLinkNetworkOptions {
    String bindHost();

    void setBindHost(String host);

    Optional<String> advertiseHost();

    void setAdvertiseHost(String host);
}
