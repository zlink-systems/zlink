plugins {
    application
}

dependencies {
    implementation(project("${path.substringBefore(":Server")}:Shared"))
    implementation(project("${path.substringBefore(":Server")}:Server:Configuration"))
    implementation(zlinkLibs.zlink.framework.core)
    implementation(zlinkLibs.zlink.framework.spring.boot.starter)
    implementation(zlinkLibs.zlink.framework.locations.redis)
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

application {
    mainClass.set("systems.zlink.samples.supportchat.server.support.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
