package dev.dongeronimo.arreconstructor

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.updatePadding
import dev.dongeronimo.arreconstructor.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        goFullscreen()

        // The debug overlay is the only thing that cares about insets. The
        // surface underneath is meant to be full bleed, cutout included, so it
        // deliberately gets no inset handling at all.
        ViewCompat.setOnApplyWindowInsetsListener(binding.sampleText) { view, insets ->
            val safe = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            view.updatePadding(left = safe.left, top = safe.top, right = safe.right)
            insets
        }

        // The instance was already created by JNI_OnLoad, when the companion
        // object below loaded the library. This only reads back the outcome.
        // The same report goes to logcat under the tag "ARReconstructor".
        binding.sampleText.text = vulkanInstanceReport().report
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // The bars come back on their own after an app switch, a notification
        // shade pull or a permission dialog, so re-assert on every focus gain
        // instead of assuming onCreate settled it.
        if (hasFocus) {
            goFullscreen()
        }
    }

    /**
     * Hands the whole display to the renderer.
     *
     * Two separate things happen here, and only doing one of them is the usual
     * mistake. setDecorFitsSystemWindows(false) stops the decor view from
     * shrinking the content to fit the bars — without it the SurfaceView is
     * built for a shorter rectangle than the screen, and the swapchain follows.
     * Hiding the bars is what actually gets them off the pixels.
     */
    private fun goFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)

        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            // Sticky immersive. An edge swipe brings the bars back as a
            // transient overlay that hides itself again, without resizing the
            // window — which matters here, because a resize means a
            // surfaceChanged and a swapchain rebuild every single time.
            systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
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
