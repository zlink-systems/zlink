plugins {
    application
}

dependencies {
    implementation(project(":TopologySupport"))
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("org.springframework.boot:spring-boot-starter-actuator:3.5.14")
}

application {
    applicationName = "observability-ops-delay"
    mainClass.set("systems.zlink.e2e.observabilityops.delay.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
