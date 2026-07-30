package dev.dongeronimo.arreconstructor

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.widget.TextView
import dev.dongeronimo.arreconstructor.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // The instance was already created by JNI_OnLoad, when the companion
        // object below loaded the library. This only reads back the outcome.
        // The same report goes to logcat under the tag "ARReconstructor".
        binding.sampleText.text = vulkanInstanceReport().report
    }

    /**
     * A native method that is implemented by the 'arreconstructor' native library,
     * which is packaged with this application.
     */
    external fun stringFromJNI(): String

    /**
     * Reports the VkInstance built by JNI_OnLoad: API versions, whether the
     * validation layer was found, and which extensions are enabled. `ok` is false
     * when the instance could not be created, and `report` says why.
     */
    external fun vulkanInstanceReport(): NativeSelfTestResult

    /**
     * Runs the native Vulkan 1.3 + VMA self-test. Implemented in vulkan_check.cpp.
     */
    external fun runVulkanSelfTest(): NativeSelfTestResult

    companion object {
        // Used to load the 'arreconstructor' library on application startup.
        init {
            System.loadLibrary("arreconstructor")
        }
    }
}