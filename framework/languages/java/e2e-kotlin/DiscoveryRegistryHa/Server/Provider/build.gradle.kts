plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-kotlin:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

kotlin {
    jvmToolchain(22)
}

application {
    applicationName = "discovery-registry-ha-kotlin-provider"
    mainClass.set("systems.zlink.e2e.kotlin.discoveryregistryha.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
