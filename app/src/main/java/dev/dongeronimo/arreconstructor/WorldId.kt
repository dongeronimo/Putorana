package dev.dongeronimo.arreconstructor

import androidx.annotation.StringRes

/**
 * The worlds the app can show, in the order the menu lists them.
 *
 * The menu is built from this enum rather than from a layout, so adding a world
 * is one entry here and nothing else. A hand written row per world in
 * activity_main.xml would be a second list to keep in step with this one, and
 * the failure mode of that going wrong is a menu entry that silently switches
 * to the wrong scene.
 *
 * The FIRST entry is the world the app starts on, twice over: [WorldMenu] ticks
 * it before anything is picked, and the native side defaults to the matching
 * WorldKind. Both have to agree, and they do because both say HelloWorld.
 *
 * @param nativeId what identifies this world to the renderer. Must match
 *   putorana::worlds::WorldKind in WorldRegistry.h. Spelled out rather than
 *   taken from [ordinal] so that reordering the menu cannot silently switch to
 *   the wrong scene. [BEHAVIOURS] has an id the native side knows and refuses,
 *   because the world is not written yet: picking it moves the tick, logs, and
 *   leaves the current scene alone.
 */
enum class WorldId(@StringRes val label: Int, val nativeId: Int) {
    HELLO_WORLD(R.string.world_hello_world, 0),
    BEHAVIOURS(R.string.world_behaviours, 1),
    OPEN_CHISEL(R.string.world_open_chisel, 2),
}
