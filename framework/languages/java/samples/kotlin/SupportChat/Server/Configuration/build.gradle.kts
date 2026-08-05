plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project("${path.substringBefore(":Server")}:Shared"))
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
    implementation("org.springframework.boot:spring-boot:3.5.14")
}

kotlin {
    jvmToolchain(22)
}
