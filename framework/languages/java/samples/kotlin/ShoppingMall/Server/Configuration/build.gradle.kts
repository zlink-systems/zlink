plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
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
    api(sampleProject("Shared"))
    api("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    api("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    api("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
    api("org.springframework.boot:spring-boot:3.5.14")
    implementation(kotlin("stdlib"))
}

kotlin {
    jvmToolchain(22)
}
