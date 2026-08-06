package dev.dongeronimo.arreconstructor

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import dev.dongeronimo.arreconstructor.databinding.ItemWorldMenuBinding

/**
 * The hamburger menu that picks the world.
 *
 * Three ordinary views in the activity's layout, and not a PopupMenu or a
 * DrawerLayout, which is the whole reason this class exists. Both of those put
 * the menu in a window of its own; that window takes focus, taking focus drops
 * sticky immersive mode, and the system bars come back over the surface for as
 * long as the menu is up. A panel that is just another child of the
 * ConstraintLayout has no window and no focus, so opening it costs the renderer
 * nothing: not a bar flicker, and certainly not a swapchain rebuild.
 *
 * Closing is three separate paths and they all end in [close]: the button
 * itself, a tap on the scrim, and picking a row. The back gesture is a fourth,
 * routed in by the activity through [onOpenChanged].
 *
 * @param button the hamburger. Toggles the panel.
 * @param scrim full screen, clickable, hidden while the menu is closed. This is
 *   what turns a tap anywhere else on the screen into a close.
 * @param panel the empty container the rows are inflated into.
 * @param onOpenChanged fires on every open and close. The activity uses it to
 *   arm its back callback, so back closes the menu instead of leaving the app.
 * @param onWorldSelected fires after the menu has already closed and moved its
 *   tick, so the handler only has the world switch itself to do.
 */
class WorldMenu(
    private val button: View,
    private val scrim: View,
    private val panel: ViewGroup,
    private val onOpenChanged: (Boolean) -> Unit,
    private val onWorldSelected: (WorldId) -> Unit,
) {

    private val rows = mutableMapOf<WorldId, ItemWorldMenuBinding>()

    var isOpen = false
        private set

    /**
     * Which world the menu shows as current. Setting it moves the tick and
     * nothing else. This class does not know how to load a world, and the
     * caller stays the only thing that does.
     */
    var selected = WorldId.entries.first()
        set(value) {
            field = value
            refreshTicks()
        }

    init {
        val inflater = LayoutInflater.from(panel.context)
        for (world in WorldId.entries) {
            val row = ItemWorldMenuBinding.inflate(inflater, panel, true)
            row.label.setText(world.label)
            row.root.setOnClickListener {
                // Close first: the handler may take long enough to build a world
                // that a menu still on screen would read as a missed tap.
                close()
                selected = world
                onWorldSelected(world)
            }
            rows[world] = row
        }
        // The property's initialiser ran before any of the rows existed, so its
        // setter had nothing to tick.
        refreshTicks()

        button.setOnClickListener { if (isOpen) close() else open() }
        scrim.setOnClickListener { close() }
    }

    fun open() = setOpen(true)

    fun close() = setOpen(false)

    private fun setOpen(open: Boolean) {
        if (open == isOpen) {
            return
        }
        isOpen = open

        // The scrim switches instantly while the panel fades. It is invisible
        // either way, and a scrim that lingered for the length of the fade would
        // eat the first tap after a close.
        scrim.visibility = if (open) View.VISIBLE else View.GONE

        // Whatever the previous open or close left running: without this a fast
        // double tap ends with the panel hidden but its alpha animating up, or
        // visible at alpha 0.
        panel.animate().cancel()
        if (open) {
            panel.alpha = 0f
            panel.visibility = View.VISIBLE
            panel.animate().alpha(1f).setDuration(FADE_MS).start()
        } else {
            panel.animate()
                .alpha(0f)
                .setDuration(FADE_MS)
                .withEndAction { panel.visibility = View.GONE }
                .start()
        }

        onOpenChanged(open)
    }

    private fun refreshTicks() {
        rows.forEach { (world, row) ->
            row.check.visibility = if (world == selected) View.VISIBLE else View.INVISIBLE
        }
    }

    private companion object {
        /** Long enough to read as a fade, short enough that nobody waits for it. */
        const val FADE_MS = 120L
    }
}
