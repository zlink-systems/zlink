plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("org.springframework.boot:spring-boot-starter-web:3.5.14")
}

application {
    applicationName = "registry-messaging-object-client"
    mainClass.set("systems.zlink.e2e.registrymessaging.objectclient.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
