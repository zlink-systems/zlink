import org.gradle.jvm.application.tasks.CreateStartScripts

plugins {
    application
}

fun sampleProject(name: String) = project("${sampleRootPath()}:$name")

fun sampleRootPath(): String {
    val serverIndex = path.indexOf(":Server")
    return if (serverIndex >= 0) {
        path.substring(0, serverIndex)
    } else {
        path.substringBeforeLast(":", "")
    }
}

dependencies {
    implementation(sampleProject("Shared"))
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.springframework.boot:spring-boot-starter-web:3.5.14")
    implementation("io.netty:netty-buffer:4.1.100.Final")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

application {
    mainClass.set("systems.zlink.samples.tictactoe.server.api.ApiProgram")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

val playStartScripts by tasks.registering(CreateStartScripts::class) {
    applicationName = "tictactoe-play"
    mainClass.set("systems.zlink.samples.tictactoe.server.play.PlayProgram")
    classpath = files(tasks.named("jar"), configurations.runtimeClasspath)
    defaultJvmOpts = application.applicationDefaultJvmArgs
    outputDir = layout.buildDirectory.dir("play-start-scripts").get().asFile
}

tasks.named<Sync>("installDist") {
    dependsOn(playStartScripts)
    from(playStartScripts) { into("bin") }
}
