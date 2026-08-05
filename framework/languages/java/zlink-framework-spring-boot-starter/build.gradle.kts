plugins {
    `java-library`
    `maven-publish`
}

description = "ZLink Framework Java Spring Boot starter"

java {
    modularity.inferModulePath.set(true)
}

dependencies {
    implementation(project(":zlink-framework-binding-internal"))
    api(project(":zlink-framework-core"))
    api(project(":zlink-http-client"))
    api("org.springframework.boot:spring-boot-autoconfigure:3.5.14")
    api("org.springframework:spring-context")
    api("org.springframework.boot:spring-boot-actuator:3.5.14")
    compileOnlyApi("io.micrometer:micrometer-core:1.15.8")
    testImplementation("io.micrometer:micrometer-core:1.15.8")
    testImplementation(project(":zlink-framework-testkit"))
}
