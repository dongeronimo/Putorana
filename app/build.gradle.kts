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
            }
        }
        ndk {
            // Único alvo: device arm64 físico. AR não roda no emulador x86.
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

// Shaders são compilados em build time, nunca no app. O script lê
// assets/shaders e escreve SPIR-V em src/main/assets/shaders, que é o que o
// Gradle empacota.
//
// Exec falha o build quando o processo sai com código != 0, que é o default e é
// exatamente o que se quer: erro de compilação de GLSL tem que parar a build, e
// não deixá-la empacotar o .spv bom da vez anterior — o sintoma disso seria um
// shader que se comporta como a versão de antes do conserto.
val compileShaders = tasks.register<Exec>("compileShaders") {
    group = "build"
    description = "Compila assets/shaders/*.vert|frag para SPIR-V em src/main/assets/shaders"

    val script = rootProject.file("tools/compile_shaders.py")
    val sources = rootProject.file("assets/shaders")
    val output = file("src/main/assets/shaders")

    workingDir = rootProject.projectDir
    // O launcher do Windows chama-se "python"; em Linux/macOS "python" pode não
    // existir ou ser o 2.x.
    val python = if (System.getProperty("os.name").startsWith("Windows")) "python" else "python3"
    commandLine(python, script.absolutePath)

    // Up-to-date checking do Gradle, além do incremental do próprio script: sem
    // isto a task roda em toda build, e o script vira ruído no log.
    inputs.dir(sources).withPathSensitivity(PathSensitivity.RELATIVE)
    inputs.file(script)
    outputs.dir(output)
}

// preBuild é a primeira task do módulo, então isto garante que os .spv existem
// antes de qualquer merge de assets.
tasks.named("preBuild") {
    dependsOn(compileShaders)
}

dependencies {

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.constraintlayout)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
}