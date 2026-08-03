#ifndef PUTORANA_AR_SUBSYSTEM_H
#define PUTORANA_AR_SUBSYSTEM_H

#include <jni.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

/**
 * ARCore, and nothing else.
 *
 * ## The rule this namespace exists to enforce
 *
 * Nothing in here includes volk.h, and nothing in here may. The products below
 * are plain memory and plain numbers; turning them into VkImages is the job of
 * putorana::graphics::CameraFeed, on the other side of that line. The
 * separation is not tidiness — the two halves have different lifetimes (see
 * below), so a class that held both would have no correct destructor.
 *
 * Because the rule is structural rather than a matter of discipline, it cannot
 * quietly rot: a VkImage member would not compile.
 *
 * ## Lifetimes, and why they do not match the renderer's
 *
 * A Device dies every time the app is backgrounded and is rebuilt on the way
 * back, taking the whole World with it. An ArSession must NOT follow it:
 * destroying a session throws away tracking, and with it every anchor and
 * everything reconstructed so far. So this object lives at the same level as the
 * VkInstance — created once, paused and resumed with the ACTIVITY, destroyed
 * only when the process is.
 *
 * ## Threads
 *
 * Two touch it. Create/Resume/Pause come from the UI thread, driven by the
 * activity's lifecycle; Update comes from the render thread, once per frame.
 * ARCore's session is not thread-safe across those, so every entry point below
 * takes the same mutex. It is uncontended in the steady state — the only overlap
 * is the moment the activity pauses while a frame is in flight, which is exactly
 * the race worth paying a lock to rule out.
 * */
namespace putorana::ar {

/**
 * One camera frame's CPU image, in the YUV_420_888 family ARCore delivers.
 *
 * ## Pointers, not copies, and what that costs the caller
 *
 * `y` and `uv` point into memory owned by an ArImage that this object holds. It
 * is released on the NEXT Update, so the rule for callers is simple and
 * absolute: consume it before then, and never store the pointers. In practice
 * that means memcpy'ing into a staging buffer inside the same frame, which is
 * what CameraFeed does.
 *
 * ## Strides are not widths
 *
 * yRowStride is >= width and uvRowStride is >= width, both of them padding the
 * driver chose. Copying `width * height` bytes in one go is the bug this field
 * exists to prevent; the rows have to be copied one at a time whenever the
 * stride does not equal the width.
 *
 * ## The chroma plane
 *
 * YUV_420_888 nominally has three planes, but on Android the U and V planes are
 * almost always two interleaved views of ONE block of memory, two bytes apart.
 * That block is what `uv` points at, at half width and half height, and it is
 * uploaded verbatim as a two-channel texture.
 *
 * Which of the two bytes is which is NOT fixed: NV12 puts U first, NV21 puts V
 * first, and both occur in the wild. Rather than guess — the symptom of guessing
 * is a picture with the reds and blues swapped, which reads as a colour-space
 * bug and sends you looking in the wrong place — the order is detected from the
 * plane pointers and reported here.
 * */
struct CameraImage {
    int32_t width = 0;
    int32_t height = 0;

    const uint8_t* y = nullptr;
    int32_t yRowStride = 0;

    /** Half width, half height, two bytes per texel. */
    const uint8_t* uv = nullptr;
    int32_t uvRowStride = 0;

    /** True when the first byte of each pair is V (NV21) rather than U (NV12). */
    bool vFirst = false;
};

/**
 * Where ARCore believes the device is, and how it is held.
 *
 * Both poses ARCore offers have this shape, and WHICH ONE you are holding is
 * the thing to keep straight — see CameraFrame::displayPose and
 * CameraFrame::sensorPose. They differ by a rotation about the view axis of a
 * multiple of 90 degrees, which means picking the wrong one produces a result
 * that is not subtly off but sideways.
 *
 * Only meaningful while tracking. Before the first successful localisation, and
 * whenever tracking is lost, the numbers are stale rather than wrong-but-close,
 * and moving a camera to them would jump the whole scene.
 * */
struct CameraPose {
    /** Unit quaternion in ARCore's raw order: qx, qy, qz, qw. */
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float translation[3] = {};
};

/**
 * A pinhole camera's intrinsic parameters, in pixels, for one specific image.
 *
 * ## What the four numbers mean
 *
 * They are the whole of the 3D-to-2D map. A point (X, Y, Z) in camera space,
 * with Z measured ALONG THE PRINCIPAL AXIS (the direction the lens points),
 * lands on the pixel
 *
 *     u = fx * (X / Z) + cx
 *     v = fy * (Y / Z) + cy
 *
 * `fx` and `fy` are the focal length expressed in pixel widths and pixel
 * heights. They differ from each other only when the sensor's pixels are not
 * square, which on a phone they essentially always are — so expect fx ~= fy and
 * treat a large gap between them as a bug rather than as a property of the
 * hardware. `cx`, `cy` are the principal point: where the optical axis actually
 * pierces the sensor, which is NEAR the centre of the image but not exactly at
 * it, and the difference is why ARCore's projection matrix has off-diagonal
 * terms (see ArCamera.cpp).
 *
 * ## Why the resolution is part of it
 *
 * All four numbers are in pixels OF A PARTICULAR IMAGE. The same lens described
 * against a 640x480 image and against a 160x120 one gives fx four times apart.
 * So intrinsics without the dimensions they were measured at are not a
 * description of anything, and `width`/`height` travel with them for that
 * reason — they are what lets a reader (or a rescale) tell the two apart.
 * */
struct Intrinsics {
    /** Focal length in pixels, horizontal and vertical. */
    float fx = 0.0f;
    float fy = 0.0f;

    /** Principal point in pixels, from the top-left of the image. */
    float cx = 0.0f;
    float cy = 0.0f;

    /** The image these are expressed against. Meaningless without it. */
    int32_t width = 0;
    int32_t height = 0;
};

/**
 * One frame's depth map: how far away the world is, per pixel.
 *
 * ## The units, and the one that is not obvious
 *
 * Each sample is an unsigned 16-bit count of MILLIMETRES, 0 to 65535 (about 65
 * metres), and a zero means "no estimate here" rather than "zero distance".
 *
 * The part worth reading twice: it is the distance ALONG THE CAMERA'S PRINCIPAL
 * AXIS, not the length of the ray to the point. It is Z, not range. That is the
 * convenient case — unprojecting is then two multiplies and no square root (see
 * recon/README.md) — but code written for a time-of-flight sensor, which
 * usually reports range, is wrong here in a way that bulges the reconstruction
 * outward at the edges of the frame while looking perfect at the centre.
 *
 * ARCore's own accuracy note: best between 0.5m and 15m, usable to 25m, with
 * error growing QUADRATICALLY with distance. That quadratic is not a footnote —
 * it is the reason a TSDF integrator weights near samples above far ones.
 *
 * ## Borrowed, like the camera image
 *
 * `millimetres` points into an ArImage this object holds and releases on the
 * NEXT Update. Same rule as CameraImage: consume it inside the frame, never
 * store the pointer.
 *
 * ## Two strides, one of which is in the wrong unit for the pointer
 *
 * `rowStrideBytes` is what ARCore reports, and ARCore reports BYTES, while the
 * pointer is uint16. Row y therefore starts at
 *
 *     millimetres + y * (rowStrideBytes / 2)
 *
 * and indexing it as `millimetres[y * width + x]` is only correct when the rows
 * happen not to be padded. The unit is in the name so that the division is hard
 * to forget.
 * */
struct DepthImage {
    int32_t width = 0;
    int32_t height = 0;

    /** Millimetres along the principal axis. 0 means no estimate. */
    const uint16_t* millimetres = nullptr;

    /** Distance between row starts, in BYTES, not in uint16s. */
    int32_t rowStrideBytes = 0;

    /**
     * Intrinsics expressed against THIS image's dimensions.
     *
     * Not what ARCore handed over: it describes the CPU image, which is several
     * times larger. Rescaled on the way in, because intrinsics belonging to a
     * different resolution than the pixels they are used with is the single
     * easiest way to build a reconstruction that is a consistent, plausible,
     * completely wrong size.
     * */
    Intrinsics intrinsics;
};

/**
 * Everything one Update produced. Valid until the next one.
 * */
struct CameraFrame {
    CameraImage image;

    /**
     * The projection ARCore wants virtual content drawn with, column-major.
     *
     * OpenGL conventions, which is what the C API produces everywhere: +Y up and
     * clip Z in [-1,1]. Vulkan wants neither, so whoever renders with it owes
     * both conversions — see ArCamera.
     *
     * Not something to compute instead. It comes from the physical sensor's
     * intrinsics and the display geometry ARCore was told about, and a
     * hand-built frustum that is close but not equal puts the virtual geometry
     * in a slightly different one from the camera image behind it. That does not
     * look like a wrong field of view; it looks like objects sliding against the
     * world whenever the phone moves.
     * */
    float projection[16] = {};

    /**
     * The pose to RENDER with: ARCore's display-oriented pose.
     *
     * +X right, +Y up, -Z into the scene, with "right" and "up" meaning what
     * they mean to someone holding the phone — the display rotation is already
     * folded in. That is what makes it agree with `projection` above, and what
     * makes it the one to write into a scene graph.
     *
     * Read `tracking` before believing it.
     * */
    CameraPose displayPose;

    /**
     * The pose to RECONSTRUCT with: ARCore's physical camera pose.
     *
     * Same origin, same axis convention (+X right, +Y up, -Z forward), but
     * "right" and "up" are relative to the SENSOR's readout order rather than
     * to the display. ARCore's header states the two differ by a local rotation
     * about Z by a multiple of 90 degrees.
     *
     * This is the one that belongs with `depth` and `imageIntrinsics`, and the
     * reason is that those two describe UNROTATED images. ARCore says so
     * outright: ArCamera_getImageIntrinsics returns "the unrotated and uncropped
     * intrinsics for the image (CPU) stream", and the AR_COORDINATES_2D_IMAGE_*
     * spaces are documented as the CPU image while the VIEW ones are explicitly
     * "display-rotated". Pixels that were never rotated must be unprojected with
     * a pose that was never rotated either.
     *
     * Using `displayPose` here instead does not go subtly wrong. In portrait it
     * lays the reconstructed room on its side.
     * */
    CameraPose sensorPose;

    /**
     * Intrinsics for the CPU camera image — `image` below, not `depth`.
     *
     * Kept even though `depth.intrinsics` is what the reconstruction reads,
     * because the rescale that produces the latter is only checkable against
     * the former. When a reconstruction comes out uniformly too large or too
     * small, these two side by side are where the factor shows up.
     * */
    Intrinsics imageIntrinsics;

    /**
     * This frame's depth map, or a default-constructed one when there is none.
     *
     * `millimetres` null is the normal state on a device without depth support,
     * before tracking has converged, and for the first frames after a resume.
     * It is not an error and nothing should log about it per frame.
     * */
    DepthImage depth;

    /** True only when ARCore reports AR_TRACKING_STATE_TRACKING this frame. */
    bool tracking = false;

    /**
     * The view-to-image mapping, as an affine basis: the images of the three
     * points (0,0), (1,0) and (0,1) of VIEW-normalised space, laid out
     * origin.u, origin.v, uAxis.u, uAxis.v, vAxis.u, vAxis.v.
     *
     * This is what makes the feed appear the right way up and the right way
     * round. The camera sensor has a fixed orientation and a fixed aspect ratio,
     * neither of which is the screen's, and the mapping between them changes on
     * every rotation. ARCore owns that mapping — the alternative is to rebuild it
     * from the display rotation and two aspect ratios by hand, and get it subtly
     * wrong on the first tablet.
     *
     * A BASIS rather than three finished texture coordinates, for two reasons.
     * The three points handed to ARCore stay inside the unit square, so nothing
     * depends on how it treats out-of-range input. And the space it describes is
     * the VIEW's — what the user is looking at — which is NOT the space a
     * fullscreen triangle is drawn in whenever the swapchain carries a
     * preTransform. Composing the two is the renderer's job, because the
     * preTransform is the renderer's business and this namespace does not know
     * Vulkan exists.
     *
     * Evaluate as: image = origin + u * (uAxis - origin) + v * (vAxis - origin).
     * It is affine, so that extrapolates exactly outside the unit square.
     * */
    float viewToImage[6] = {};
};

class Subsystem {
public:
    /**
     * Creates the session. `context` must be an Android Context — the Activity
     * is what the samples pass and what the install flow needs.
     *
     * Fails, rather than being tolerated, when ARCore is not installed or the
     * device is unsupported: the manifest declares this app AR Required, so
     * there is no degraded mode to fall back to. The caller reports it and the
     * renderer keeps drawing without a background.
     *
     * Does NOT start the camera. Nothing happens until Resume.
     * */
    static std::unique_ptr<Subsystem> Create(JNIEnv* env, jobject context, std::string& error);

    ~Subsystem();

    Subsystem(const Subsystem&) = delete;
    Subsystem& operator=(const Subsystem&) = delete;

    /**
     * Starts the camera. Call from the activity's onResume, and only once the
     * CAMERA permission has actually been granted — ARCore fails here rather
     * than prompting.
     * */
    bool Resume(std::string& error);

    /** Stops the camera. From onPause, and it must not be skipped. */
    void Pause();

    /**
     * Tells ARCore the shape of the viewport it is projecting into.
     *
     * `rotation` is the DISPLAY rotation — Surface.ROTATION_0/90/180/270 as an
     * int, not the swapchain's preTransform, which is a related but different
     * quantity. Everything ARCore hands back in view coordinates, the UVs above
     * included, is wrong until this has been called with the truth.
     * */
    void SetDisplayGeometry(int32_t rotation, int32_t width, int32_t height);

    /**
     * The clip planes ARCore builds CameraFrame::projection with, in metres.
     *
     * They live here rather than being passed per call because the projection is
     * computed once per Update, inside the lock, along with everything else the
     * frame produced. Whoever owns the camera sets them once and forgets.
     * */
    void SetClipPlanes(float nearPlane, float farPlane);

    /**
     * One session tick: advances tracking and fetches the camera image.
     *
     * Call once per frame, BEFORE anything records a command buffer. It blocks
     * until a new camera image arrives or ARCore's built-in 66ms timeout
     * expires, which is why it belongs beside the world's Update and not inside
     * a render pass — see the frame loop.
     *
     * Releases the previous frame's image, which invalidates every pointer
     * handed out by the last call.
     * */
    void Update();

    /**
     * The most recent frame, borrowed under lock.
     *
     * ## Why this is not a plain getter
     *
     * CameraFrame carries pointers into an ArImage this object owns, and Pause —
     * which arrives on the UI thread, at any moment, because the user pressed
     * Home — releases that image. A frame being recorded on the render thread at
     * the same instant would be memcpy'ing from freed memory. The window is
     * real: onPause runs well before the surface is destroyed, so the render
     * thread is still going.
     *
     * Holding the subsystem's lock for as long as the guard lives closes it. The
     * cost is that Pause waits for the copy, which is a memcpy of about half a
     * megabyte, and the benefit is that it cannot happen underneath one.
     *
     * get() answers null when there is no frame — the normal state for the first
     * handful of frames after a Resume, while the camera stack comes up, which
     * ARCore reports as "not yet available" rather than as an error.
     * */
    class FrameGuard {
    public:
        FrameGuard(std::unique_lock<std::mutex> lock, const CameraFrame* frame)
                : lock_(std::move(lock)), frame_(frame) {}

        const CameraFrame* get() const { return frame_; }

    private:
        std::unique_lock<std::mutex> lock_;
        const CameraFrame* frame_ = nullptr;
    };

    FrameGuard AcquireFrame() const;

    /** True between a successful Resume and the next Pause. */
    bool running() const { return running_; }

    /**
     * Whether this device can produce depth at all, decided once at Create.
     *
     * False is a legitimate device, not a failure: the Depth API is a
     * per-device capability and ARCore runs perfectly well without it. What is
     * NOT legitimate is reconstructing from a session that answered false here,
     * so this is the flag whoever builds the reconstruction has to consult
     * before deciding it has anything to do.
     * */
    bool depthSupported() const { return depthSupported_; }

private:
    Subsystem() = default;

    /** Drops the held ArImages, if any. Caller holds the mutex. */
    void ReleaseImage();

    /** Poses, intrinsics, projection, tracking state, UV basis. Caller holds the mutex. */
    void ReadCamera();

    /** The CPU image. Best effort — leaves the planes null when unavailable. */
    void ReadCameraImage();

    /** The depth map. Best effort — leaves it null whenever there is none. */
    void ReadDepthImage();

    mutable std::mutex mutex_;

    // Opaque on purpose: declaring these as ArSession* would drag
    // arcore_c_api.h into every translation unit that includes this header, and
    // this header is included by the renderer.
    void* session_ = nullptr;
    void* arFrame_ = nullptr;
    void* image_ = nullptr;
    void* depthImage_ = nullptr;
    /** Allocated once and reused: ArPose_create every frame would be litter. */
    void* pose_ = nullptr;
    /** Likewise, and reused for both the image and the depth reads. */
    void* intrinsics_ = nullptr;

    float nearPlane_ = 0.1f;
    float farPlane_ = 1000.0f;

    CameraFrame frame_;
    bool hasFrame_ = false;
    bool running_ = false;

    /** Answered by ArSession_isDepthModeSupported at Create, and fixed after. */
    bool depthSupported_ = false;

    /** So a device that cannot produce CPU images says so once, not every frame. */
    bool warnedAboutImage_ = false;

    /** The same, for depth: one line per reason, not sixty a second. */
    bool warnedAboutDepth_ = false;

    /**
     * So the first depth map that arrives gets its dimensions and rescaled
     * intrinsics printed exactly once.
     *
     * This is not decoration. The rescale in ReadDepthImage assumes the depth
     * map covers the same field of view as the CPU image, and ARCore documents
     * the depth resolution as depending on "the device and its display aspect
     * ratio" — wording that leaves room for a crop rather than a clean scale.
     * The two aspect ratios in that one log line are what settles it on a real
     * device, which is the only place it can be settled.
     * */
    bool logDepthOnce_ = true;

    /** Set by SetDisplayGeometry, so one set of UVs is logged after each change. */
    bool logUvOnce_ = false;
};

/**
 * The one subsystem, reachable by name from the JNI entry points — the same
 * shape as instance_holder, and for the same reason.
 *
 * Deliberately NOT device_holder's shape: that one constructs on first use,
 * which is right for something with no failure mode. Creating an ArSession can
 * fail for half a dozen reasons the user needs to be told about, so it is
 * created explicitly and Get() may answer null forever after.
 * */
namespace subsystem_holder {

/** Builds it. False with the reason in `error`; Get() then stays null. */
bool Create(JNIEnv* env, jobject context, std::string& error);

/** The subsystem, or null when it was never created or creation failed. */
Subsystem* Get();

/** Tears it down. After this the camera is off and Get() is null again. */
void Destroy();

} // namespace subsystem_holder

} // namespace putorana::ar

#endif //PUTORANA_AR_SUBSYSTEM_H
