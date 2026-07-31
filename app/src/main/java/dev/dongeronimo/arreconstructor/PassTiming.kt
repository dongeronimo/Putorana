package dev.dongeronimo.arreconstructor

/**
 * How long one named GPU scope took, in milliseconds.
 *
 * Built natively by [NativeProfiler.passTimings]. The names are whatever the
 * render passes declared — see GpuProfiler.h — so this side never has to know
 * what passes exist, and adding one changes no Kotlin at all.
 *
 * [milliseconds] is GPU time, not wall time, and it is exponentially smoothed.
 * It is also a couple of frames stale by construction: a timestamp cannot be
 * read until the submission carrying it has retired.
 */
data class PassTiming(val name: String, val milliseconds: Float)
