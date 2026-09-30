plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":server-support"))
    implementation(project(":spot-server"))
    implementation(project(":kotlin:shared"))
    implementation(zlinkLibs.zlink.framework.kotlin)
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
}

application {
    applicationName = "spot-server"
    mainClass.set("systems.zlink.framework.perf.servers.spot.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

tasks.withType<Jar>().configureEach { archiveBaseName.set("kotlin-${project.name}") }
