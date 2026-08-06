plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "dev.dongeronimo.arreconstructor"
    compileSdk = 36
    // r28+ alinha os segmentos LOAD em 16 KB por padrão; o NDK padrão do AGP 8.12 (r27) não.
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "dev.dongeronimo.arreconstructor"
        minSdk = 33
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                // CMake fetches the same AAR the `implementation` below does, to
                // pull libarcore_sdk_c.so out of it and link against that
                // (see src/main/cpp/CMakeLists.txt). The version comes from here
                // so the catalog stays the only place that decides it: if the two
                // ever disagreed, the build would link against one version and
                // package another.
                arguments += "-DARCORE_VERSION=${libs.versions.arcore.get()}"
            }
        }
        ndk {
            // The only target: a physical arm64 device. AR does not run on the x86 emulator.
            abiFilters += "arm64-v8a"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildFeatures {
        viewBinding = true
    }
}

// Shaders are compiled at build time, never by the app. The script reads
// assets/shaders and writes SPIR-V into src/main/assets/shaders, which is what
// Gradle packages.
//
// Exec fails the build when the process exits non-zero, which is the default and
// exactly what is wanted: a GLSL compile error has to stop the build rather than
// let it package the good .spv from last time. The symptom of that would be a
// shader behaving like the version from before the fix.
val compileShaders = tasks.register<Exec>("compileShaders") {
    group = "build"
    description = "Compiles assets/shaders/*.vert|frag to SPIR-V in src/main/assets/shaders"

    val script = rootProject.file("tools/compile_shaders.py")
    val sources = rootProject.file("assets/shaders")
    val output = file("src/main/assets/shaders")

    workingDir = rootProject.projectDir
    // The Windows launcher is called "python"; on Linux and macOS "python" may
    // not exist at all, or may be the 2.x one.
    val python = if (System.getProperty("os.name").startsWith("Windows")) "python" else "python3"
    commandLine(python, script.absolutePath)

    // Gradle's up-to-date checking, on top of the script's own incremental pass:
    // without this the task runs on every build and the script becomes log noise.
    inputs.dir(sources).withPathSensitivity(PathSensitivity.RELATIVE)
    inputs.file(script)
    outputs.dir(output)
}

// preBuild is the module's first task, so this guarantees the .spv files exist
// before any asset merge.
tasks.named("preBuild") {
    dependsOn(compileShaders)
}

dependencies {

    // Brings in classes.jar (ArCoreApk, for the install and permission flow), the
    // manifest merge, and the packaging of jni/**/*.so into the APK. Linking
    // natively against that .so is handled separately, by CMake.
    implementation(libs.arcore)

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.constraintlayout)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
}