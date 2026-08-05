module zlink.stream.connector {
    requires transitive systems.zlink;
    requires transitive com.fasterxml.jackson.databind;
    requires com.fasterxml.jackson.datatype.jsr310;
    requires transitive io.netty.buffer;
    requires transitive io.netty.codec.http;
    requires transitive io.netty.handler;
    requires transitive io.netty.transport;
    requires org.lz4.java;
    requires micrometer.core;
    requires java.net.http;
    requires java.logging;

    exports systems.zlink.stream.connector;
}
