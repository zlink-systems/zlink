plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.9.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.9.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.9.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("org.springframework.boot:spring-boot-starter-web:3.5.14")
}

application {
    applicationName = "pub-sub-subscriber"
    mainClass.set("systems.zlink.e2e.pubsub.subscriber.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
