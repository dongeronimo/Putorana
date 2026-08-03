#include "Subsystem.h"

#include "arcore_c_api.h"

#include <android/log.h>

#include <atomic>
#include <cmath>

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
ArCameraIntrinsics* Intrin(void* handle) { return static_cast<ArCameraIntrinsics*>(handle); }

/**
 * Copies an ArPose out into our own struct.
 *
 * ArPose_getPoseRaw's seven floats are the quaternion FIRST and the translation
 * second — qx, qy, qz, qw, tx, ty, tz — which is the order CameraPose mirrors so
 * that this stays a straight copy with nothing to get backwards.
 * */
void ReadPoseInto(ArSession* session, ArPose* pose, CameraPose& out) {
    float raw[7] = {};
    ArPose_getPoseRaw(session, pose, raw);
    out.rotation[0] = raw[0];
    out.rotation[1] = raw[1];
    out.rotation[2] = raw[2];
    out.rotation[3] = raw[3];
    out.translation[0] = raw[4];
    out.translation[1] = raw[5];
    out.translation[2] = raw[6];
}

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

    // Depth, which is the whole input to the reconstruction and is NOT on by
    // default.
    //
    // Asked about first rather than set optimistically. AR_DEPTH_MODE_AUTOMATIC
    // on a device that cannot do it makes ArSession_configure fail with
    // AR_ERROR_UNSUPPORTED_CONFIGURATION, and that failure would take the whole
    // session down — camera feed, tracking and all — over a capability the rest
    // of the app does not need. So the query decides, the flag is remembered,
    // and a device without depth still gets everything else.
    //
    // AUTOMATIC and not RAW_DEPTH_ONLY. Automatic is the smoothed, filled-in
    // map: every pixel carries an estimate, temporally fused across frames.
    // Raw is sparse — zero wherever ARCore is unsure — and comes with a separate
    // confidence image. Raw is arguably the better input for a TSDF, which does
    // its own fusion and would rather not integrate someone else's guesses
    // twice; the reason it is not the choice here is that we cannot yet tell
    // whether that is true on this hardware, and a dense map is the one that
    // makes the first reconstruction visibly work or visibly not. Revisit with
    // measurements, not with taste.
    int32_t depthSupported = 0;
    ArSession_isDepthModeSupported(session, AR_DEPTH_MODE_AUTOMATIC, &depthSupported);
    subsystem->depthSupported_ = depthSupported != 0;
    if (subsystem->depthSupported_) {
        ArConfig_setDepthMode(session, config, AR_DEPTH_MODE_AUTOMATIC);
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "RECONPROBE device does not support AR_DEPTH_MODE_AUTOMATIC; "
                            "no depth, so no reconstruction");
    }

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

    ArCameraIntrinsics* intrinsics = nullptr;
    ArCameraIntrinsics_create(session, &intrinsics);
    subsystem->intrinsics_ = intrinsics;

    // RECONPROBE here too: this is the gate, and if it says UNSUPPORTED then the
    // depth lines never print at all and their absence would otherwise look like
    // a bug somewhere further down.
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "RECONPROBE depth mode %s",
                        subsystem->depthSupported_ ? "SUPPORTED" : "UNSUPPORTED");
    return subsystem;
}

Subsystem::~Subsystem() {
    // No lock: nothing may still be calling into this while it is being
    // destroyed, and taking one here would only hide it if something were.
    ReleaseImage();
    if (intrinsics_ != nullptr) {
        ArCameraIntrinsics_destroy(Intrin(intrinsics_));
    }
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
    // After ReadCamera, and that is an ordering requirement rather than a
    // preference: the depth intrinsics are derived by rescaling the CPU image's,
    // so frame_.imageIntrinsics has to be this frame's before this runs.
    ReadDepthImage();
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
    // The depth image is a separate acquisition with its own pool, and ARCore
    // answers AR_ERROR_RESOURCE_EXHAUSTED once an app holds too many without
    // giving them back. Releasing it here, next to the camera image, is what
    // keeps the two on one code path instead of two that have to agree.
    if (depthImage_ != nullptr) {
        ArImage_release(Image(depthImage_));
        depthImage_ = nullptr;
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

    // Both poses, every frame, because the two halves of this app want
    // different ones and neither is derivable from the other without redoing
    // the display-rotation reasoning ARCore has already done.
    //
    // The display-oriented pose folds in the display rotation, so it agrees
    // with the projection above: it is what RENDERING uses.
    // The physical pose does not, so it agrees with the CPU image and the
    // unrotated intrinsics below: it is what RECONSTRUCTION uses.
    // ArCamera_getPose's own documentation says the two differ by a local
    // rotation about Z of a multiple of 90 degrees.
    ArCamera_getDisplayOrientedPose(session, camera, Pose(pose_));
    ReadPoseInto(session, Pose(pose_), frame_.displayPose);

    ArCamera_getPose(session, camera, Pose(pose_));
    ReadPoseInto(session, Pose(pose_), frame_.sensorPose);

    // "Unrotated and uncropped intrinsics for the image (CPU) stream", in
    // ARCore's words — which is precisely why they belong with sensorPose and
    // not with displayPose. Re-read every frame because the header says they
    // may change per frame; autofocus moves the focal length.
    ArCameraIntrinsics* intrinsics = Intrin(intrinsics_);
    ArCamera_getImageIntrinsics(session, camera, intrinsics);
    ArCameraIntrinsics_getFocalLength(session, intrinsics, &frame_.imageIntrinsics.fx,
                                      &frame_.imageIntrinsics.fy);
    ArCameraIntrinsics_getPrincipalPoint(session, intrinsics, &frame_.imageIntrinsics.cx,
                                         &frame_.imageIntrinsics.cy);
    ArCameraIntrinsics_getImageDimensions(session, intrinsics, &frame_.imageIntrinsics.width,
                                          &frame_.imageIntrinsics.height);

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

void Subsystem::ReadDepthImage() {
    frame_.depth = DepthImage{};

    if (!depthSupported_) {
        return;
    }
    // ARCore refuses depth outright while not tracking (AR_ERROR_NOT_TRACKING),
    // and rightly so: a depth map without a pose to place it at is not data,
    // it is a picture. Checking here keeps that out of the error path below,
    // where it would be indistinguishable from a real failure.
    if (!frame_.tracking) {
        return;
    }

    ArSession* session = Session(session_);
    ArFrame* frame = Frame(arFrame_);

    ArImage* image = nullptr;
    const ArStatus acquired = ArFrame_acquireDepthImage16Bits(session, frame, &image);
    if (acquired != AR_SUCCESS) {
        // NOT_YET_AVAILABLE is the ordinary state while ARCore accumulates the
        // motion it needs to estimate depth at all — the user has to move the
        // phone before there is anything to have. Not an error, and not worth a
        // line sixty times a second.
        if (acquired != AR_ERROR_NOT_YET_AVAILABLE && !warnedAboutDepth_) {
            warnedAboutDepth_ = true;
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "ArFrame_acquireDepthImage16Bits failed: %s",
                                StatusName(acquired));
        }
        return;
    }
    depthImage_ = image;

    DepthImage& out = frame_.depth;
    ArImage_getWidth(session, image, &out.width);
    ArImage_getHeight(session, image, &out.height);

    const uint8_t* bytes = nullptr;
    int32_t length = 0;
    ArImage_getPlaneData(session, image, 0, &bytes, &length);
    ArImage_getPlaneRowStride(session, image, 0, &out.rowStrideBytes);
    if (bytes == nullptr || out.width <= 0 || out.height <= 0) {
        out = DepthImage{};
        return;
    }

    // The cast is safe in a way that is worth stating rather than assuming.
    // D_16 is a single 16-bit plane, and ARCore hands back the start of a row —
    // an address the hardware itself required to be 2-byte aligned to produce
    // 16-bit pixels at. Little-endian matches every arm64 Android device, and
    // the ABI this project builds is arm64 only.
    out.millimetres = reinterpret_cast<const uint16_t*>(bytes);

    // Intrinsics belong to the CPU image, which is several times larger than
    // this one, and NOT the same shape. Getting from one to the other is a crop
    // followed by a scale, and both halves matter.
    //
    // This used to be a plain multiply by the width and height ratios, on the
    // assumption that depth covered the same field of view as the CPU image.
    // A device settled it, and the assumption was wrong:
    //
    //     depth 160x90 (16:9) from a CPU image of 640x480 (4:3)
    //
    // The failure is quiet and specific. Scaling fy by height/height preserves
    // the FOV of the image you scaled FROM, so the maths ends up believing the
    // depth map sees 57.2 degrees vertically when it really sees 44.5. Nothing
    // looks broken; the room simply comes out with its floor and ceiling flared
    // away from the camera, more so toward the top and bottom of frame.
    //
    // The right model. The sensor image is cropped to the depth map's aspect
    // ratio about its centre, then resampled uniformly:
    //
    //     wider depth  (16:9 from 4:3) -> rows are cropped, full width kept
    //     taller depth              -> columns are cropped, full height kept
    //
    // Crucially the crop is derived from the two aspect ratios rather than
    // hardcoded, so a device that crops the other way, or not at all, lands in
    // the same three lines. And because the crop restores the aspect ratio
    // before the resample, scaleX and scaleY come out EQUAL — which is the
    // invariant the probe below asserts. If they ever disagree, the crop is not
    // centred and this model is wrong.
    //
    // Verified on the measured numbers: cropTop = 60, scale = 0.25,
    // fx 439.73 -> 109.93, fy 440.19 -> 110.05, cx 314.88 -> 78.72,
    // cy 241.35 -> 45.34. The off-centre principal point (+1.35px of 480)
    // survives as +0.34px of 90 — exactly a quarter — which is independent
    // confirmation that the crop really is centred.
    // Hoisted out of the block below only so the probe can report them.
    float cropLeft = 0.0f;
    float cropTop = 0.0f;
    float cropScaleX = 0.0f;
    float cropScaleY = 0.0f;

    const Intrinsics& src = frame_.imageIntrinsics;
    if (src.width > 0 && src.height > 0 && out.width > 0 && out.height > 0) {
        const float srcW = static_cast<float>(src.width);
        const float srcH = static_cast<float>(src.height);
        const float depthAspect = static_cast<float>(out.width) / static_cast<float>(out.height);

        // The region of the CPU image the depth map actually covers, in CPU
        // image pixels. One of these is always the full extent: cropping only
        // ever removes from the axis that has too much.
        float cropW = srcW;
        float cropH = srcH;
        if (depthAspect > srcW / srcH) {
            cropH = srcW / depthAspect;   // depth is wider: lose rows
        } else {
            cropW = srcH * depthAspect;   // depth is taller: lose columns
        }
        cropLeft = (srcW - cropW) * 0.5f;
        cropTop = (srcH - cropH) * 0.5f;

        const float scaleX = static_cast<float>(out.width) / cropW;
        const float scaleY = static_cast<float>(out.height) / cropH;
        cropScaleX = scaleX;
        cropScaleY = scaleY;

        out.intrinsics.fx = src.fx * scaleX;
        out.intrinsics.fy = src.fy * scaleY;
        // The principal point moves with the crop before it scales: it is a
        // position in the image, not a length, so the new origin has to come off
        // it first. This is the half nobody remembers, and it is invisible on a
        // centred sensor because cx - cropLeft happens to land back in the
        // middle. It does not stay invisible on a sensor that is off-centre,
        // which every real one is.
        out.intrinsics.cx = (src.cx - cropLeft) * scaleX;
        out.intrinsics.cy = (src.cy - cropTop) * scaleY;
        out.intrinsics.width = out.width;
        out.intrinsics.height = out.height;

    }

    if (logDepthOnce_) {
        logDepthOnce_ = false;

        // Every line below carries kProbe. Filtering by log tag is not enough in
        // practice: ARCore is extremely chatty under its own tags, and on a busy
        // device the one line that matters scrolls past in a wall of unrelated
        // output. This token appears nowhere else on the system.
        //
        // The checks are also evaluated HERE rather than printed as raw numbers
        // for someone to interpret. Every one of them is a fixed comparison
        // against a fixed expectation, so having the device state the verdict
        // removes the step where the numbers get read wrong.
        constexpr const char* kProbe = "RECONPROBE";

        const float depthAspect = static_cast<float>(out.width) / static_cast<float>(out.height);
        const float imageAspect = src.height > 0
                ? static_cast<float>(src.width) / static_cast<float>(src.height)
                : 0.0f;
        const bool aspectsAgree = imageAspect > 0.0f && std::abs(depthAspect - imageAspect) <= 0.01f;

        // D_16 is 16 bits per sample, so a tightly packed row is width * 2 bytes.
        // Anything larger means padding, and every read has to go through
        // rowStrideBytes rather than width — which the code does, but that would
        // be by luck rather than on purpose, so it is worth knowing.
        const int32_t tightStride = out.width * 2;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "%s depth %dx%d, stride %d bytes, tight would be %d -> %s",
                            kProbe, out.width, out.height, out.rowStrideBytes, tightStride,
                            out.rowStrideBytes == tightStride ? "TIGHT" : "PADDED");

        // A mismatch here is NOT a failure any more — it is the measured reality
        // on this hardware (16:9 depth from a 4:3 image) and the rescale handles
        // it. What it changes is which line below is load bearing: with a crop
        // in play, the scale invariant is the thing that has to hold.
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "%s aspect depth %.4f vs CPU image %.4f (image %dx%d) -> %s",
                            kProbe, depthAspect, imageAspect, src.width, src.height,
                            aspectsAgree ? "MATCH, no crop" : "MISMATCH, cropping");

        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "%s crop left %.1f top %.1f px of the %dx%d image, then scale "
                            "x %.4f y %.4f -> %s",
                            kProbe, cropLeft, cropTop, src.width, src.height,
                            cropScaleX, cropScaleY,
                            std::abs(cropScaleX - cropScaleY) < 1e-3f ? "UNIFORM" : "NON-UNIFORM");

        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "%s intrinsics image fx %.2f fy %.2f cx %.2f cy %.2f "
                            "-> depth fx %.2f fy %.2f cx %.2f cy %.2f",
                            kProbe, src.fx, src.fy, src.cx, src.cy,
                            out.intrinsics.fx, out.intrinsics.fy,
                            out.intrinsics.cx, out.intrinsics.cy);

        bool fovConsistent = false;
        if (out.intrinsics.fx > 0.0f && out.intrinsics.fy > 0.0f) {
            // The field of view the rescaled intrinsics imply:
            //
            //     fov = 2 * atan(width / (2 * fx))
            //
            // This is the number that catches a wrong fx. A wrong focal length
            // does not produce a broken-looking reconstruction — it produces a
            // room that is uniformly the wrong size, which is much harder to
            // notice later than a nonsense angle is to notice now.
            constexpr float kRadToDeg = 57.2957795f;
            const float hfov = 2.0f * std::atan(static_cast<float>(out.width)
                                                / (2.0f * out.intrinsics.fx)) * kRadToDeg;
            const float vfov = 2.0f * std::atan(static_cast<float>(out.height)
                                                / (2.0f * out.intrinsics.fy)) * kRadToDeg;
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "%s fov horizontal %.1f deg, vertical %.1f deg "
                                "(a phone rear camera is roughly 60-70 horizontal) -> %s",
                                kProbe, hfov, vfov,
                                (hfov > 45.0f && hfov < 85.0f) ? "PLAUSIBLE" : "SUSPECT");

            // The real check, and the one that would have caught the bug this
            // code was just fixed for. For a rectilinear lens the two focal
            // lengths and the image shape are not independent:
            //
            //     tan(hfov/2) / tan(vfov/2)  ==  width / height
            //
            // because both sides are (width/2)/fx over (height/2)/fy, and fx and
            // fy are equal for square pixels. It ties fx, fy, width and height
            // together in one number, so no single one of them can be wrong
            // without showing up here. With the old height-ratio rescale this
            // came out 1.335 against a 1.778 image — a 25% error that every
            // other line in this probe reported as fine.
            const float fovRatio = std::tan(hfov * 0.5f / kRadToDeg)
                                   / std::tan(vfov * 0.5f / kRadToDeg);
            fovConsistent = std::abs(fovRatio - depthAspect) < 0.02f * depthAspect;
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "%s fov ratio %.4f vs image aspect %.4f -> %s",
                                kProbe, fovRatio, depthAspect,
                                fovConsistent ? "CONSISTENT" : "INCONSISTENT");

            // Off-centre is normal and physical — it is why ARCore's projection
            // matrix has terms off the diagonal. Wildly off-centre is not.
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "%s principal point %+.1f, %+.1f px from centre of %dx%d",
                                kProbe,
                                out.intrinsics.cx - static_cast<float>(out.width) * 0.5f,
                                out.intrinsics.cy - static_cast<float>(out.height) * 0.5f,
                                out.width, out.height);
        }

        // The verdict is no longer "do the aspects match" — they do not on this
        // hardware, and that is handled. It is whether the derived intrinsics are
        // self-consistent: a uniform scale (so the crop model holds) and a field
        // of view that agrees with the image shape (so fx and fy agree with each
        // other).
        const bool scaleUniform = std::abs(cropScaleX - cropScaleY) < 1e-3f;
        if (scaleUniform && fovConsistent) {
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "%s VERDICT OK: crop %.0f,%.0f then uniform scale %.4f; "
                                "intrinsics self-consistent.",
                                kProbe, cropLeft, cropTop, cropScaleX);
        } else {
            // Loud, because every unprojected point is wrong if this fires and
            // nothing else in the app will look unwell.
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "%s VERDICT FAIL: scale uniform %s, fov consistent %s. The crop "
                                "model in ReadDepthImage does not describe this device. See "
                                "putorana/recon/README.md.",
                                kProbe, scaleUniform ? "yes" : "NO",
                                fovConsistent ? "yes" : "NO");
        }
    }
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
