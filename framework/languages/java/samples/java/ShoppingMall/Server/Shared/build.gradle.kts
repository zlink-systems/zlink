plugins {
    `java-library`
}

tasks.jar {
    archiveBaseName.set("shoppingmall-server-shared")
}

fun sampleProject(name: String) = project("${sampleRootPath()}:$name")

fun sampleRootPath(): String {
    val serverIndex = path.indexOf(":Server")
    return if (serverIndex >= 0) path.substring(0, serverIndex) else path.substringBeforeLast(":", "")
}

dependencies {
    api(sampleProject("Shared"))
    api(sampleProject("Server:Configuration"))
    api("io.lettuce:lettuce-core:6.3.2.RELEASE")
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
