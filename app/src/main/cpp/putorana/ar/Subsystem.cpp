#include "Subsystem.h"

#include "arcore_c_api.h"

#include <android/log.h>

#include <atomic>

namespace putorana::ar {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

/**
 * The unit basis of view-normalised space: the origin, one unit along u, one
 * unit along v. (0,0) is the top-left of the viewport.
 *
 * Three points, and all of them inside the unit square on purpose. What comes
 * back is enough to reconstruct the whole affine map, and reconstructing it
 * ourselves is what keeps this independent of how ArFrame_transformCoordinates2d
 * treats coordinates outside its domain — which is not documented, and which a
 * fullscreen triangle's off-screen corners would otherwise be relying on.
 * */
constexpr float kViewBasis[6] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
};

ArSession* Session(void* handle) { return static_cast<ArSession*>(handle); }
ArFrame* Frame(void* handle) { return static_cast<ArFrame*>(handle); }
ArImage* Image(void* handle) { return static_cast<ArImage*>(handle); }
ArPose* Pose(void* handle) { return static_cast<ArPose*>(handle); }

const char* StatusName(ArStatus status) {
    switch (status) {
        case AR_SUCCESS: return "AR_SUCCESS";
        case AR_ERROR_INVALID_ARGUMENT: return "AR_ERROR_INVALID_ARGUMENT";
        case AR_ERROR_FATAL: return "AR_ERROR_FATAL";
        case AR_ERROR_SESSION_PAUSED: return "AR_ERROR_SESSION_PAUSED";
        case AR_ERROR_SESSION_NOT_PAUSED: return "AR_ERROR_SESSION_NOT_PAUSED";
        case AR_ERROR_NOT_TRACKING: return "AR_ERROR_NOT_TRACKING";
        case AR_ERROR_RESOURCE_EXHAUSTED: return "AR_ERROR_RESOURCE_EXHAUSTED";
        case AR_ERROR_NOT_YET_AVAILABLE: return "AR_ERROR_NOT_YET_AVAILABLE";
        case AR_ERROR_CAMERA_PERMISSION_NOT_GRANTED:
            return "AR_ERROR_CAMERA_PERMISSION_NOT_GRANTED";
        case AR_ERROR_DEADLINE_EXCEEDED: return "AR_ERROR_DEADLINE_EXCEEDED";
        case AR_ERROR_UNSUPPORTED_CONFIGURATION: return "AR_ERROR_UNSUPPORTED_CONFIGURATION";
        case AR_UNAVAILABLE_ARCORE_NOT_INSTALLED: return "AR_UNAVAILABLE_ARCORE_NOT_INSTALLED";
        case AR_UNAVAILABLE_DEVICE_NOT_COMPATIBLE: return "AR_UNAVAILABLE_DEVICE_NOT_COMPATIBLE";
        case AR_UNAVAILABLE_APK_TOO_OLD: return "AR_UNAVAILABLE_APK_TOO_OLD";
        case AR_UNAVAILABLE_SDK_TOO_OLD: return "AR_UNAVAILABLE_SDK_TOO_OLD";
        default: break;
    }
    return "AR_ERROR (unnamed)";
}

} // namespace

std::unique_ptr<Subsystem> Subsystem::Create(JNIEnv* env, jobject context, std::string& error) {
    auto subsystem = std::unique_ptr<Subsystem>(new Subsystem());

    ArSession* session = nullptr;
    const ArStatus created = ArSession_create(env, context, &session);
    if (created != AR_SUCCESS) {
        error = std::string("ArSession_create failed: ") + StatusName(created);
        return nullptr;
    }
    subsystem->session_ = session;

    ArConfig* config = nullptr;
    ArConfig_create(session, &config);

    // The point of this line is NOT the hardware buffer — nothing here reads
    // one. It is that this mode makes ARCore stop wanting an OpenGL texture.
    //
    // The default, AR_TEXTURE_UPDATE_MODE_BIND_TO_TEXTURE_EXTERNAL_OES, expects
    // a GL_TEXTURE_EXTERNAL_OES name handed over by ArSession_setCameraTextureName,
    // and this process has no GL context at all to make one in. Selecting
    // EXPOSE_HARDWARE_BUFFER makes those texture names "ignored" by the header's
    // own wording, which is exactly the opt-out wanted: the camera image is then
    // consumed purely through ArFrame_acquireCameraImage, on the CPU.
    //
    // The day the zero-copy path is worth building, it goes here, behind this
    // same interface, and this comment is the only thing that has to change.
    ArConfig_setTextureUpdateMode(session, config, AR_TEXTURE_UPDATE_MODE_EXPOSE_HARDWARE_BUFFER);

    // Blocking is the default and is what is wanted: it paces the renderer to
    // the camera, which is the rate at which anything on screen can actually
    // change. It waits at most ARCore's built-in 66ms before handing back the
    // previous frame, so it cannot wedge the loop.
    ArConfig_setUpdateMode(session, config, AR_UPDATE_MODE_BLOCKING);

    const ArStatus configured = ArSession_configure(session, config);
    ArConfig_destroy(config);
    if (configured != AR_SUCCESS) {
        error = std::string("ArSession_configure failed: ") + StatusName(configured);
        return nullptr;
    }

    ArFrame* frame = nullptr;
    ArFrame_create(session, &frame);
    subsystem->arFrame_ = frame;

    ArPose* pose = nullptr;
    ArPose_create(session, nullptr, &pose);
    subsystem->pose_ = pose;

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "AR subsystem created");
    return subsystem;
}

Subsystem::~Subsystem() {
    // No lock: nothing may still be calling into this while it is being
    // destroyed, and taking one here would only hide it if something were.
    ReleaseImage();
    if (pose_ != nullptr) {
        ArPose_destroy(Pose(pose_));
    }
    if (arFrame_ != nullptr) {
        ArFrame_destroy(Frame(arFrame_));
    }
    if (session_ != nullptr) {
        // Pause first: destroying a running session is legal but leaves the
        // camera held for longer than it needs to be.
        if (running_) {
            ArSession_pause(Session(session_));
        }
        ArSession_destroy(Session(session_));
    }
}

bool Subsystem::Resume(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_ == nullptr || running_) {
        return true;
    }
    const ArStatus status = ArSession_resume(Session(session_));
    if (status != AR_SUCCESS) {
        error = std::string("ArSession_resume failed: ") + StatusName(status);
        return false;
    }
    running_ = true;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "AR session resumed");
    return true;
}

void Subsystem::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_ == nullptr || !running_) {
        return;
    }
    // The held image belongs to a session that is about to stop producing them.
    // Dropping it here rather than on the next Update means the camera buffer
    // goes back to ARCore before the camera is released, instead of after.
    ReleaseImage();
    hasFrame_ = false;

    const ArStatus status = ArSession_pause(Session(session_));
    if (status != AR_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "ArSession_pause failed: %s",
                            StatusName(status));
    }
    running_ = false;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "AR session paused");
}

void Subsystem::SetDisplayGeometry(int32_t rotation, int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_ == nullptr) {
        return;
    }
    ArSession_setDisplayGeometry(Session(session_), rotation, width, height);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "AR display geometry: rotation %d, %dx%d",
                        rotation, width, height);
    // The UVs that come back are the only observable consequence of this call,
    // and getting them wrong looks like a stretched picture rather than like a
    // geometry bug. Logging one set after each change is what turns that into
    // something readable.
    logUvOnce_ = true;
}

void Subsystem::Update() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_ == nullptr || !running_) {
        return;
    }

    // Before the update, not after: ArSession_update is documented to
    // invalidate what the previous frame handed out, and holding images past it
    // is what exhausts ARCore's pool.
    ReleaseImage();
    hasFrame_ = false;

    const ArStatus status = ArSession_update(Session(session_), Frame(arFrame_));
    if (status != AR_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "ArSession_update failed: %s",
                            StatusName(status));
        return;
    }

    // The camera's half always succeeds once the update did — pose, projection
    // and tracking state exist from the first frame, even while the pose is
    // still meaningless. The IMAGE is the part that can be missing, for several
    // frames after a resume and on devices that do not offer CPU images at all,
    // so the two are read separately and a frame with no picture is still a
    // perfectly good frame to take a projection matrix from.
    ReadCamera();
    hasFrame_ = true;
    ReadCameraImage();
}

void Subsystem::SetClipPlanes(float nearPlane, float farPlane) {
    std::lock_guard<std::mutex> lock(mutex_);
    nearPlane_ = nearPlane;
    farPlane_ = farPlane;
}

Subsystem::FrameGuard Subsystem::AcquireFrame() const {
    std::unique_lock<std::mutex> lock(mutex_);
    const CameraFrame* frame = hasFrame_ ? &frame_ : nullptr;
    return FrameGuard(std::move(lock), frame);
}

void Subsystem::ReleaseImage() {
    if (image_ != nullptr) {
        ArImage_release(Image(image_));
        image_ = nullptr;
    }
}

void Subsystem::ReadCamera() {
    ArSession* session = Session(session_);
    ArFrame* frame = Frame(arFrame_);

    // The ArCamera is a reference type: acquired per frame, released here. It is
    // NOT the ArSession's camera object in any lasting sense — holding one past
    // the frame it came from is what ArCamera_release exists to prevent.
    ArCamera* camera = nullptr;
    ArFrame_acquireCamera(session, frame, &camera);

    ArTrackingState trackingState = AR_TRACKING_STATE_STOPPED;
    ArCamera_getTrackingState(session, camera, &trackingState);
    frame_.tracking = trackingState == AR_TRACKING_STATE_TRACKING;

    ArCamera_getProjectionMatrix(session, camera, nearPlane_, farPlane_, frame_.projection);

    // The display-oriented pose, which already folds in the display rotation, so
    // it agrees with the projection above. The raw sensor pose does not, and
    // using it would tilt the world by ninety degrees in landscape.
    ArCamera_getDisplayOrientedPose(session, camera, Pose(pose_));
    float raw[7] = {};
    ArPose_getPoseRaw(session, Pose(pose_), raw);
    frame_.pose.rotation[0] = raw[0];
    frame_.pose.rotation[1] = raw[1];
    frame_.pose.rotation[2] = raw[2];
    frame_.pose.rotation[3] = raw[3];
    frame_.pose.translation[0] = raw[4];
    frame_.pose.translation[1] = raw[5];
    frame_.pose.translation[2] = raw[6];

    ArCamera_release(camera);

    // Recomputed every frame rather than only when ArFrame_getDisplayGeometryChanged
    // reports a change. It is six floats through an affine transform — cheaper
    // than the branch that would avoid it, and it cannot go stale.
    ArFrame_transformCoordinates2d(session, frame, AR_COORDINATES_2D_VIEW_NORMALIZED, 3,
                                   kViewBasis, AR_COORDINATES_2D_IMAGE_NORMALIZED,
                                   frame_.viewToImage);

    if (logUvOnce_) {
        logUvOnce_ = false;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "AR view->image: origin (%.3f,%.3f) u (%.3f,%.3f) v (%.3f,%.3f)",
                            frame_.viewToImage[0], frame_.viewToImage[1], frame_.viewToImage[2],
                            frame_.viewToImage[3], frame_.viewToImage[4], frame_.viewToImage[5]);
    }
}

void Subsystem::ReadCameraImage() {
    ArSession* session = Session(session_);
    ArFrame* frame = Frame(arFrame_);

    frame_.image = CameraImage{};

    ArImage* image = nullptr;
    const ArStatus acquired = ArFrame_acquireCameraImage(session, frame, &image);
    if (acquired != AR_SUCCESS) {
        // NOT_YET_AVAILABLE is the normal state for the first frames after a
        // resume, while the camera stack comes up, so it is not worth a line of
        // logcat sixty times a second. Anything else is worth saying once.
        if (acquired != AR_ERROR_NOT_YET_AVAILABLE && !warnedAboutImage_) {
            warnedAboutImage_ = true;
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "ArFrame_acquireCameraImage failed: %s", StatusName(acquired));
        }
        return;
    }
    image_ = image;

    int32_t planeCount = 0;
    ArImage_getNumberOfPlanes(session, image, &planeCount);
    if (planeCount < 3) {
        if (!warnedAboutImage_) {
            warnedAboutImage_ = true;
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "camera image has %d planes, expected 3 (YUV_420_888)",
                                planeCount);
        }
        return;
    }

    CameraImage& out = frame_.image;
    ArImage_getWidth(session, image, &out.width);
    ArImage_getHeight(session, image, &out.height);

    int32_t unusedLength = 0;
    ArImage_getPlaneData(session, image, 0, &out.y, &unusedLength);
    ArImage_getPlaneRowStride(session, image, 0, &out.yRowStride);

    // Planes 1 and 2 are U and V. On every Android device seen in the wild they
    // are two windows onto ONE interleaved block two bytes apart, and which of
    // them starts first is what distinguishes NV12 from NV21. Comparing the
    // pointers answers that without having to trust a format enum, and picking
    // the lower of the two is where the interleaved block actually begins.
    const uint8_t* u = nullptr;
    const uint8_t* v = nullptr;
    ArImage_getPlaneData(session, image, 1, &u, &unusedLength);
    ArImage_getPlaneData(session, image, 2, &v, &unusedLength);
    ArImage_getPlaneRowStride(session, image, 1, &out.uvRowStride);

    int32_t uvPixelStride = 0;
    ArImage_getPlanePixelStride(session, image, 1, &uvPixelStride);
    if (uvPixelStride != 2) {
        // Fully planar 4:2:0. Uploading it as a two-channel texture would read
        // the U plane as if it were interleaved and produce a picture that is
        // half green. Refusing is better than drawing that.
        if (!warnedAboutImage_) {
            warnedAboutImage_ = true;
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "chroma pixel stride is %d, expected 2 (semi-planar NV12/NV21)",
                                uvPixelStride);
        }
        return;
    }

    out.vFirst = v < u;
    out.uv = out.vFirst ? v : u;
}

namespace subsystem_holder {

namespace {

std::unique_ptr<Subsystem> g_subsystem;

/**
 * The same pointer again, atomically.
 *
 * Unlike instance_holder, this one is published from a different thread than it
 * is read from: Create runs on the UI thread when the camera permission comes
 * back, while the frame loop calls Get on the render thread sixty times a
 * second. A bare pointer written by one and read by the other is a data race in
 * the language's own terms, whatever the hardware happens to do with it — and
 * the release/acquire pair is also what publishes everything the constructor
 * wrote, not merely the pointer's own bytes.
 * */
std::atomic<Subsystem*> g_published{nullptr};

} // namespace

bool Create(JNIEnv* env, jobject context, std::string& error) {
    if (g_published.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    std::unique_ptr<Subsystem> created = Subsystem::Create(env, context, error);
    if (created == nullptr) {
        return false;
    }
    g_subsystem = std::move(created);
    g_published.store(g_subsystem.get(), std::memory_order_release);
    return true;
}

Subsystem* Get() { return g_published.load(std::memory_order_acquire); }

void Destroy() {
    // Unpublished before it is destroyed, so a render thread that has not yet
    // read the pointer cannot pick up one that is about to dangle. A thread
    // already INSIDE a call keeps its own reference alive; this only runs from
    // JNI_OnUnload, by which point the render thread is long gone.
    g_published.store(nullptr, std::memory_order_release);
    g_subsystem.reset();
}

} // namespace subsystem_holder

} // namespace putorana::ar
