plugins {
    `java-library`
}

dependencies {
    api(project(":shared"))
    api(zlinkLibs.zlink.framework.core)
    api(zlinkLibs.zlink.framework.locations.redis)
    api(zlinkLibs.zlink.framework.spring.boot.starter)
    api(zlinkLibs.zlink.bindings)
    api("org.springframework.boot:spring-boot-starter:3.5.14")
    api("io.micrometer:micrometer-core:1.15.8")
    api("com.fasterxml.jackson.datatype:jackson-datatype-jdk8:2.17.2")
    api("com.fasterxml.jackson.datatype:jackson-datatype-jsr310:2.17.2")
}
