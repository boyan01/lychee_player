group = "tech.soit.lychee.lychee_player"
version = "1.0-SNAPSHOT"

buildscript {
    val kotlinVersion = "2.4.0"
    repositories {
        google()
        mavenCentral()
    }

    dependencies {
        classpath("com.android.tools.build:gradle:9.1.0")
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:$kotlinVersion")
    }
}

// The core comes from the Android SDK's AAR, tech.soit.lychee:lychee
// (liblychee.so with its FFmpeg, and the frame driver), published into
// sdk/android/build/repo by `sdk/android/gradlew :lychee:publish`. The app
// resolves the plugin's dependencies, so every project gets the repository.
val lycheeRepo = file("../../sdk/android/build/repo")
if (!lycheeRepo.isDirectory) {
    logger.warn("lychee_player: no $lycheeRepo; run sdk/android/gradlew -p sdk/android :lychee:publish")
}
val lycheeVersion = Regex("""(?m)^version: (\S+)""")
    .find(file("../pubspec.yaml").readText())!!.groupValues[1]

rootProject.allprojects {
    repositories {
        google()
        mavenCentral()
        maven { url = uri(lycheeRepo) }
    }
}

plugins {
    id("com.android.library")
}

android {
    namespace = "tech.soit.lychee.lychee_player"

    compileSdk = 36

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/kotlin")
        }
    }

    defaultConfig {
        minSdk = 26
    }
}

dependencies {
    implementation("tech.soit.lychee:lychee:$lycheeVersion")
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}
