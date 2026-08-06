plugins {
    application
    id("org.jetbrains.kotlin.jvm")
    id("org.jetbrains.kotlin.plugin.spring")
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.10.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.2")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

application {
    applicationName = "channel-egress-kotlin-role"
    mainClass.set("systems.zlink.e2e.kotlin.channelegress.role.RoleApplicationKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

kotlin.jvmToolchain(22)
