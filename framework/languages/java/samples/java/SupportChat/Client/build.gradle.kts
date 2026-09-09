plugins {
    application
}

dependencies {
    implementation(project("${path.substringBeforeLast(":Client")}:Shared"))
    implementation(project("${path.substringBeforeLast(":Client")}:Server:Configuration"))
    implementation(zlinkLibs.zlink.stream.connector)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

application {
    mainClass.set("systems.zlink.samples.supportchat.client.Program")
}
