plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

application {
    applicationName = "channel-egress-role"
    mainClass.set("systems.zlink.e2e.channelegress.role.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
