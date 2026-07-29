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

        // Brings up Vulkan 1.3 + VMA on the real device and reports what it found.
        // The full report also goes to logcat under the tag "ARReconstructor".
        val selfTest = runVulkanSelfTest()
        binding.sampleText.text = selfTest.report
    }

    /**
     * A native method that is implemented by the 'arreconstructor' native library,
     * which is packaged with this application.
     */
    external fun stringFromJNI(): String

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