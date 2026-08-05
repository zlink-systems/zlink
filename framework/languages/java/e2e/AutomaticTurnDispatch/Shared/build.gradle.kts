// Shared Config 8 contracts and evidence.
plugins {
    `java-library`
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    //  EvidenceHttpServer가 `framework.spring.internal.runtime`의 lifecycle을 쓴다.
    api("systems.zlink:zlink-framework-spring-boot-starter:0.1.0-SNAPSHOT")
    api("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    api(zlinkLibs.zlink.bindings)
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    api("org.springframework:spring-context:6.2.18")
    api("io.micrometer:micrometer-core:1.15.8")
    api("io.lettuce:lettuce-core:7.6.0.RELEASE")
}
