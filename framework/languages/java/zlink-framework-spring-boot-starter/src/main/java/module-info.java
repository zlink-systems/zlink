module zlink.framework.spring.boot.starter {
    requires transitive systems.zlink.framework;
    requires zlink.framework.binding.internal;
    requires transitive zlink.http.client;
    requires transitive spring.boot.autoconfigure;
    requires transitive spring.context;
    requires transitive spring.boot.actuator;
    requires spring.beans;
    requires spring.core;
    requires java.logging;
    requires java.desktop;
    requires static micrometer.core;

    exports systems.zlink.framework.spring;

    opens systems.zlink.framework.spring to
        spring.beans,
        spring.context,
        spring.core;
    opens systems.zlink.framework.spring.internal.runtime to
        spring.beans,
        spring.context,
        spring.core;
}
