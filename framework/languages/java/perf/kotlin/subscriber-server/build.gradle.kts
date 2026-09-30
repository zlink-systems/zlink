plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":server-support"))
    implementation(project(":subscriber-server"))
    implementation(project(":kotlin:shared"))
    implementation(zlinkLibs.zlink.framework.kotlin)
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
}

application {
    applicationName = "subscriber-server"
    mainClass.set("systems.zlink.framework.perf.servers.subscriber.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

tasks.withType<Jar>().configureEach { archiveBaseName.set("kotlin-${project.name}") }
