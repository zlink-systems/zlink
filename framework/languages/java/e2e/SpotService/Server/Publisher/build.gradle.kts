plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.9.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.9.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.9.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

application {
    applicationName = "spot-service-publisher"
    mainClass.set("systems.zlink.e2e.spotservice.publisher.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
