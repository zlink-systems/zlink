plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(22)
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.10.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    implementation("systems.zlink:zlink-stream-connector:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

application {
    applicationName = "spot-service-kotlin-session"
    mainClass.set("systems.zlink.e2e.kotlin.spotservice.session.SessionProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
