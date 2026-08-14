plugins {
    application
}

group = "systems.zlink.e2e"
version = "0.10.0"

dependencies {
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    val zlinkBindingVersion = providers.environmentVariable("ZLINK_EXPECTED_BINDING_VERSION")
        .orElse("0.11.1")
        .get()
    implementation("systems.zlink:zlink:$zlinkBindingVersion")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

application {
    applicationName = "submit-admission-role"
    mainClass.set("systems.zlink.e2e.submitadmission.SubmitAdmissionRole")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
