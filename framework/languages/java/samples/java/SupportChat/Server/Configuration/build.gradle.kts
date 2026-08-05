plugins {
    `java-library`
}

dependencies {
    implementation(project("${path.substringBefore(":Server")}:Shared"))
    implementation("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
    implementation("org.springframework.boot:spring-boot:3.5.14")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
