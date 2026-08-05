plugins {
    application
    id("org.jetbrains.kotlin.jvm")
    id("org.jetbrains.kotlin.plugin.spring")
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.9.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.9.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.9.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.9.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
}

kotlin {
    jvmToolchain(22)
}

application {
    applicationName = "to-actor-kotlin-caller"
    mainClass.set("systems.zlink.e2e.kotlin.toactormessaging.caller.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
