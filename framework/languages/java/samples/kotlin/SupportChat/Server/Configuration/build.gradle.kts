plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project("${path.substringBefore(":Server")}:Shared"))
    implementation(zlinkLibs.zlink.framework.core)
    implementation(zlinkLibs.zlink.framework.locations.redis)
    implementation("org.springframework.boot:spring-boot:3.5.14")
}
