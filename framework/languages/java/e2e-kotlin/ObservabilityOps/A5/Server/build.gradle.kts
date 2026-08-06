plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation("systems.zlink:zlink-framework-core:0.9.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.9.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.9.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.9.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

kotlin { jvmToolchain(22) }

application {
    applicationName = "observability-ops-kotlin-a5-server"
    mainClass.set("systems.zlink.e2e.kotlin.observabilityops.a5.server.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
