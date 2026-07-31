package dev.dongeronimo.arreconstructor

import android.Manifest
import android.content.pm.PackageManager
import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.updatePadding
import dev.dongeronimo.arreconstructor.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    /** True once NativeAr.create has succeeded. Nothing else may be called before it. */
    private var arCreated = false

    /** UI thread, for the overlay poll. */
    private val timingsHandler = Handler(Looper.getMainLooper())

    /**
     * Whether the activity is between onResume and onPause.
     *
     * Needed because the permission dialog breaks the usual ordering: the grant
     * can land after onResume has already run, so the code that starts the
     * camera cannot assume it is the one being resumed.
     */
    private var activityResumed = false

    private val requestCamera =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                startAr()
            } else {
                // AR Required, so there is no degraded mode to fall back to. The
                // renderer keeps drawing the cube on its clear colour, which is
                // a good deal more informative than a black screen.
                binding.sampleText.text = "camera permission denied — no AR"
            }
        }

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

        // ARCore never prompts for the camera itself; it just fails. So the
        // permission has to be settled before the session is created.
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            startAr()
        } else {
            requestCamera.launch(Manifest.permission.CAMERA)
        }
    }

    override fun onResume() {
        super.onResume()
        activityResumed = true
        if (arCreated) {
            NativeAr.resume()
        }
        timingsHandler.post(refreshTimings)
    }

    override fun onPause() {
        activityResumed = false
        // Stop polling before anything else: the device, and with it the query
        // pools the numbers come from, is torn down on the way out.
        timingsHandler.removeCallbacks(refreshTimings)
        if (arCreated) {
            // Before super, so the camera is released on the way out rather than
            // after the framework has already begun tearing the window down.
            NativeAr.pause()
        }
        super.onPause()
    }

    /**
     * Polls the GPU timings onto the overlay.
     *
     * Polled rather than pushed from native, and deliberately: a callback per
     * frame would cross the JNI boundary thirty times a second to update a label
     * nobody can read that fast, and would have to hop to the UI thread each
     * time. Four times a second is faster than the smoothing settles anyway.
     */
    private val refreshTimings = object : Runnable {
        override fun run() {
            val timings = NativeProfiler.passTimings()
            binding.gpuTimings.text = if (timings.isEmpty()) {
                "gpu: no timings"
            } else {
                // Right-aligned to a fixed width so the numbers do not dance
                // horizontally as they change — the whole reason for a monospace
                // font here.
                timings.joinToString("\n") { "%-14s %6.2f ms".format(it.name, it.milliseconds) }
            }
            timingsHandler.postDelayed(this, TIMINGS_INTERVAL_MS)
        }
    }

    /**
     * Creates the session and, if the activity is already running, starts the
     * camera.
     *
     * The second half is what makes this callable from the permission result: by
     * then onResume has long since been and gone, so nothing else would ever
     * start the camera.
     */
    private fun startAr() {
        if (arCreated) {
            return
        }
        val created = NativeAr.create(this)
        binding.sampleText.text = created.report
        if (!created.ok) {
            return
        }
        arCreated = true
        if (activityResumed) {
            NativeAr.resume()
        }
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
        /** Four times a second: faster than the smoothing settles, slower than an eye reads. */
        private const val TIMINGS_INTERVAL_MS = 250L

        // Used to load the 'arreconstructor' library on application startup.
        init {
            System.loadLibrary("arreconstructor")
        }
    }
}
