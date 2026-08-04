#include "Reconstruction.h"

#include <open_chisel/Chisel.h>
#include <open_chisel/ProjectionIntegrator.h>
#include <open_chisel/camera/DepthImage.h>
#include <open_chisel/camera/PinholeCamera.h>

#include <android/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace putorana::recon {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

chisel::ChunkID ToChunkId(const ChunkKey& key) {
    return {key.x, key.y, key.z};
}

ChunkKey ToChunkKey(const chisel::ChunkID& id) {
    return ChunkKey{id.x(), id.y(), id.z()};
}

/**
 * ARCore's camera-to-world pose, as the extrinsic OpenChisel wants.
 *
 * Two conventions meet here and they disagree on two axes. The intrinsics --
 * and therefore everything ProjectionIntegrator does with them -- are computer
 * vision convention: +X right, +Y DOWN, +Z FORWARD out of the lens. That is why
 * the integrator rejects points with `voxelCenterInCamera.z() < 0` and reads
 * depth off `.z()` directly. ARCore's pose is OpenGL convention: +Y UP, camera
 * looking down its own -Z.
 *
 * So the transform is the pose composed with a flip of Y and Z. This is exactly
 * the same pair of sign flips Unproject.cpp applies per point, written once as a
 * matrix -- and if the two ever disagree, the point cloud and the reconstruction
 * will land in different places, which is the cheapest possible way to notice.
 * */
chisel::Transform ToExtrinsic(const ar::CameraPose& pose) {
    // glm's quat constructor takes w FIRST; ARCore's raw array has it LAST.
    const Eigen::Quaternionf rotation(pose.rotation[3], pose.rotation[0], pose.rotation[1],
                                      pose.rotation[2]);

    chisel::Transform transform = chisel::Transform::Identity();
    transform.linear() = rotation.toRotationMatrix() *
                         Eigen::DiagonalMatrix<float, 3>(1.0f, -1.0f, -1.0f);
    transform.translation() =
            Eigen::Vector3f(pose.translation[0], pose.translation[1], pose.translation[2]);
    return transform;
}

} // namespace

struct Reconstruction::Impl {
    Config config;

    std::unique_ptr<chisel::Chisel> chisel;
    chisel::ProjectionIntegrator integrator;
    chisel::PinholeCamera camera;

    /**
     * Reused across frames. chisel::DepthImage OWNS its buffer -- its destructor
     * does `delete[] data` -- so pointing it at ARCore's memory with SetData
     * would be a double free the moment the frame is released. Allocated once at
     * the depth map's size and written into instead.
     * */
    std::shared_ptr<chisel::DepthImage<float>> depth;

    /**
     * One integration weight per depth sample, derived from ARCore's confidence
     * map and handed to the integrator alongside the depth.
     *
     * A DepthImage rather than a bare float array purely so the integrator can
     * index it with the same row/column it already computed for the depth --
     * see ProjectionIntegrator::SetWeights. It holds weights, not distances.
     * */
    std::shared_ptr<chisel::DepthImage<float>> weights;

    std::mutex meshMutex;

    /**
     * The current remesh cycle: chunks taken from Chisel's dirty set, drained a
     * budget at a time. Refilled only when empty. See Remesh for what goes wrong
     * without it.
     * */
    std::deque<chisel::ChunkID> remeshQueue;

    std::vector<ChunkUpdate> changed;
    std::vector<ChunkKey> removed;

    /** Chunk IDs whose mesh was non-empty last time we looked, so removals can
     *  be reported even though OpenChisel garbage-collects silently. */
    std::unordered_map<chisel::ChunkID, bool, chisel::ChunkHasher> live;

    bool loggedFirstIntegration = false;

    /**
     * Whether the "how sparse is raw depth" line has been printed.
     *
     * Separate from loggedFirstIntegration, and the reason is a lesson: the
     * first integrated frame is the WORST possible moment to characterise the
     * depth stream. Raw depth is built from motion, so a phone that has not been
     * moved yet reports zero valid samples out of the whole image, and a probe
     * that fires once on frame one reports 0% and then never speaks again.
     * This one waits for a frame that actually has data.
     * */
    bool loggedSparsity = false;

    /** Frames where every sample was rejected, so Chisel never saw them. */
    uint32_t emptyFrames = 0;

    /**
     * Where a frame's depth samples went, and how confident the ones that
     * existed were. Both accumulate over the timing window below.
     *
     * ## The question this exists to answer
     *
     * The floor comes back in ragged patches: grout lines, skirting boards and
     * furniture edges mesh, while the clean faces of the tiles between them do
     * not, at every distance and every viewing angle. Two explanations fit that
     * equally well from the outside, and they have opposite fixes.
     *
     * Either those pixels carry a depth that we are discarding or under-weighting
     * -- in which case the gate and the weight curve are the thing to change --
     * or ARCore never produced a depth there at all. Raw depth is motion stereo
     * and a blank tile face has nothing to match, so a whole tile can come back
     * as literal zeros: not an uncertain measurement, an absent one. No weighting
     * scheme reaches an absent measurement.
     *
     * `noEstimate` separates them in one number. The histogram then says whether
     * the confidences we DO get are distributed in a way that makes any threshold
     * meaningful, or whether they pile up at one end.
     * */
    uint32_t noEstimateSamples = 0;
    uint32_t histogramSamples = 0;
    std::array<uint32_t, 8> confidenceHistogram{};

    /**
     * Samples walked in this window. Accumulated rather than derived from the
     * frame count, because frames that reject everything return before the
     * integration counter advances and would otherwise not appear in the
     * denominator -- which is precisely the case this probe is about.
     * */
    uint32_t windowSamples = 0;

    /** Integration cost, accumulated and reported once per window. */
    static constexpr uint32_t kTimingWindow = 60;
    uint64_t integrateNanosTotal = 0;
    uint32_t integrationsInWindow = 0;

    /** Chunk origin from its key alone: numVoxels * ID * resolution, which is
     *  what Chunk's constructor computes. Deterministic, so it needs no lookup
     *  and works for a chunk that has already been collected. */
    glm::vec3 OriginOf(const chisel::ChunkID& id) const {
        const float edge = static_cast<float>(config.chunkVoxels) * config.voxelMetres;
        return glm::vec3(static_cast<float>(id.x()) * edge, static_cast<float>(id.y()) * edge,
                         static_cast<float>(id.z()) * edge);
    }
};

Reconstruction::Reconstruction() : impl_(std::make_unique<Impl>()) {}
Reconstruction::~Reconstruction() = default;

std::unique_ptr<Reconstruction> Reconstruction::Create(const Config& config, std::string& error) {
    if (config.chunkVoxels <= 0 || config.voxelMetres <= 0.0f) {
        error = "Reconstruction: chunkVoxels and voxelMetres must both be positive";
        return nullptr;
    }

    auto reconstruction = std::unique_ptr<Reconstruction>(new Reconstruction());
    Impl& impl = *reconstruction->impl_;
    impl.config = config;

    const Eigen::Vector3i chunkSize(config.chunkVoxels, config.chunkVoxels, config.chunkVoxels);
    // useColor = false. Colour would double the per-voxel cost -- a ColorVoxel is
    // as wide as a DistVoxel even after the fork trimmed both -- and nothing
    // draws the reconstruction in colour. See third_party/open_chisel/README.md.
    impl.chisel = std::make_unique<chisel::Chisel>(chunkSize, config.voxelMetres, false);

    // Centroids are the per-voxel offsets within a chunk, precomputed once by
    // ChunkManager. The integrator walks them for every chunk in the frustum, so
    // it needs its own copy rather than reaching for the manager per frame.
    impl.integrator.SetCentroids(impl.chisel->GetChunkManager().GetCentroids());
    impl.integrator.SetTruncator(chisel::Truncator(config.truncationQuadratic,
                                                   config.truncationLinear,
                                                   config.truncationConstant,
                                                   config.truncationScale));
    impl.integrator.SetWeighter(chisel::Weighter(config.weight));
    impl.integrator.SetCarvingDist(config.carvingDistance);
    impl.integrator.SetCarvingEnabled(config.carvingEnabled);

    impl.camera.SetNearPlane(config.nearPlane);
    impl.camera.SetFarPlane(config.farPlane);

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "RECONPROBE reconstruction ready: %d^3 voxels per chunk at %.3f m "
                        "(%.2f m chunks), truncation |%.3g d^2 + %.3g d + %.3g| * %.3g, "
                        "carving %s, depth [%.2f, %.2f] m, confidence gate %u/255 then "
                        "weight %.2f..1.00",
                        config.chunkVoxels, config.voxelMetres,
                        config.chunkVoxels * config.voxelMetres, config.truncationQuadratic,
                        config.truncationLinear, config.truncationConstant, config.truncationScale,
                        config.carvingEnabled ? "on" : "off", config.nearPlane, config.farPlane,
                        static_cast<unsigned>(config.confidenceThreshold),
                        config.confidenceWeightFloor);
    return reconstruction;
}

void Reconstruction::Integrate(const ar::DepthImage& depth, const ar::CameraPose& pose) {
    Impl& impl = *impl_;
    if (depth.millimetres == nullptr || depth.width <= 0 || depth.height <= 0) {
        return;
    }
    const ar::Intrinsics& k = depth.intrinsics;
    if (k.fx <= 0.0f || k.fy <= 0.0f) {
        return;
    }

    if (impl.depth == nullptr || impl.depth->GetWidth() != depth.width ||
        impl.depth->GetHeight() != depth.height) {
        impl.depth = std::make_shared<chisel::DepthImage<float>>(depth.width, depth.height);
        impl.weights = std::make_shared<chisel::DepthImage<float>>(depth.width, depth.height);
        impl.integrator.SetWeights(impl.weights);
    }

    // --- millimetres to metres, and zero to NaN ---
    //
    // The second half is not cosmetic. ARCore writes 0 where it has no estimate,
    // and ProjectionIntegrator's only guard is `if (std::isnan(depth)) continue`.
    // Passing 0.0f straight through would tell it there is a surface at zero
    // distance for every pixel it could not measure -- which on a typical indoor
    // frame is a real fraction of the image, and would carve a hole through the
    // reconstruction wherever the sensor was uncertain.
    float* out = impl.depth->GetMutableData();
    const auto* base = reinterpret_cast<const uint8_t*>(depth.millimetres);
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

    // A missing confidence map means "no opinion", NOT "no confidence". A device
    // that hands over depth but refuses confidence still gets a reconstruction,
    // unfiltered, exactly as this did before the gate existed -- treating the
    // absent map as zeros everywhere would reconstruct nothing at all, which is
    // a far worse way to fail than reconstructing noisily.
    const bool useConfidence = depth.confidence != nullptr &&
                               depth.confidenceRowStrideBytes > 0 &&
                               impl.config.confidenceThreshold > 0;

    // Counted while we are walking every pixel anyway, so it costs an increment.
    // It is the cheapest possible check that the RAW stream is really what we
    // are being handed: the automatic one fills in every pixel and would read
    // ~100% valid, while raw is documented as sparse. If this comes back at 100%
    // the acquire call is still the smoothed one.
    // Confidence 0-255 to an integration weight in [floor, 1], linear over the
    // range that SURVIVES the threshold rather than over the whole 0-255 span.
    // That is what makes confidenceWeightFloor mean what its name says: the
    // weakest sample actually fused gets exactly it.
    float* weightOut = impl.weights->GetMutableData();
    const float weightFloor = std::clamp(impl.config.confidenceWeightFloor, 0.0f, 1.0f);
    const float confidenceBase = static_cast<float>(impl.config.confidenceThreshold);
    const float confidenceSpan = std::max(255.0f - confidenceBase, 1.0f);

    uint32_t validSamples = 0;
    uint32_t rejectedSamples = 0;
    uint32_t lowConfidenceSamples = 0;
    float observedMax = 0.0f;
    double weightSum = 0.0;
    for (int32_t row = 0; row < depth.height; ++row) {
        // Through the byte stride, not the width: they are equal on the device
        // this was written against and nothing promises they stay that way.
        const auto* source = reinterpret_cast<const uint16_t*>(base + row * depth.rowStrideBytes);
        // Its own stride, because it is an 8-bit plane and that one is 16-bit.
        const uint8_t* confidenceRow =
                useConfidence ? depth.confidence + static_cast<size_t>(row) *
                                                           depth.confidenceRowStrideBytes
                              : nullptr;
        float* destination = out + static_cast<size_t>(row) * depth.width;
        float* weightDestination = weightOut + static_cast<size_t>(row) * depth.width;
        for (int32_t column = 0; column < depth.width; ++column) {
            // Cleared on every path that rejects. The integrator never reads a
            // weight whose depth is NaN, so this is belt and braces -- but a
            // stale weight from the previous frame is the kind of bug that only
            // shows up as a faint bias, so it is not left to that guarantee.
            weightDestination[column] = 0.0f;

            const uint16_t millimetres = source[column];
            if (millimetres == 0) {
                destination[column] = kNaN;
                ++impl.noEstimateSamples;
                continue;
            }

            // Counted for every sample that HAS a depth, before any threshold,
            // so the histogram describes the sensor rather than our filtering.
            if (confidenceRow != nullptr) {
                ++impl.confidenceHistogram[confidenceRow[column] / 32];
                ++impl.histogramSamples;
            }

            // The confidence gate, and it is deliberately BEFORE observedMax.
            //
            // A rejected sample must not reach the frustum either. The wildest
            // readings in a raw frame -- the 13 metre one in a one metre room
            // that put the process past 1.1 GB -- are exactly the ones ARCore
            // has least confidence in, because they come from a disparity search
            // that found nothing to match. Letting them set the far plane and
            // only then discarding their depth would pay the whole cost of the
            // outlier while getting none of its geometry.
            if (confidenceRow != nullptr &&
                confidenceRow[column] < impl.config.confidenceThreshold) {
                destination[column] = kNaN;
                ++lowConfidenceSamples;
                continue;
            }

            const float metres = static_cast<float>(millimetres) * 0.001f;
            observedMax = std::max(observedMax, metres);

            // Out of range becomes NaN, exactly like "no estimate", and this is
            // the guard that keeps the app alive rather than a refinement.
            //
            // Chisel::IntegrateDepthScan does NOT use the camera's configured
            // far plane. It takes the MAXIMUM VALUE IN THE IMAGE and builds the
            // frustum from it (Chisel.h, GetStats then SetFarPlane), then
            // allocates a chunk for every chunk that frustum intersects.
            //
            // Depth is uint16 millimetres, so a single stray pixel can say
            // 65.5 metres. Frustum volume grows with the CUBE of its depth, so
            // that one pixel turns a 5-metre view worth ~190 chunks into
            // hundreds of thousands. At 32 KiB of voxels each the process was
            // past 1.1 GB and died inside Chunk::AllocateDistVoxels.
            //
            // Raw depth made it worse than the smoothed stream did, and for a
            // reason worth remembering: smoothing bounds outliers, sparsity
            // does not.
            if (metres < impl.config.nearPlane || metres > impl.config.farPlane) {
                destination[column] = kNaN;
                ++rejectedSamples;
                continue;
            }
            destination[column] = metres;
            // A frame with no confidence map fuses at full weight, which is
            // upstream's behaviour and the right fallback: absent is "no
            // opinion", and a device that will not tell us has not told us the
            // samples are bad.
            const float weight =
                    confidenceRow != nullptr
                            ? weightFloor + (1.0f - weightFloor) *
                                                    ((static_cast<float>(confidenceRow[column]) -
                                                      confidenceBase) /
                                                     confidenceSpan)
                            : 1.0f;
            weightDestination[column] = weight;
            weightSum += weight;
            ++validSamples;
        }
    }

    // Nothing survived. Bail out BEFORE Chisel sees the frame, and this is a
    // safety guard rather than an optimisation.
    //
    // IntegrateDepthScan opens with depthImage->GetStats, which skips NaN. On an
    // all-NaN image it therefore leaves its outputs at their initial values --
    // minimum = FLT_MAX, maximum = -FLT_MAX -- and hands those straight to
    // SetNearPlane and SetFarPlane. The frustum built from an inverted, infinite
    // pair of planes has a bounding box to match, and GetChunkIDsIntersecting
    // then loops over the chunk indices between two saturated integers.
    //
    // Rare before the confidence gate, because a frame with no readings at all
    // is rare. Not rare after it: a dark room, a phone still on a desk, or the
    // first seconds after a resume all produce frames where every sample is
    // below threshold.
    impl.windowSamples +=
            static_cast<uint32_t>(depth.width) * static_cast<uint32_t>(depth.height);

    if (validSamples == 0) {
        ++impl.emptyFrames;
        return;
    }

    // Re-read every frame: the header says intrinsics may change per frame, and
    // autofocus moves the focal length.
    chisel::Intrinsics intrinsics;
    intrinsics.SetFx(k.fx);
    intrinsics.SetFy(k.fy);
    intrinsics.SetCx(k.cx);
    intrinsics.SetCy(k.cy);
    impl.camera.SetIntrinsics(intrinsics);
    impl.camera.SetWidth(depth.width);
    impl.camera.SetHeight(depth.height);

    const auto integrateBegan = std::chrono::steady_clock::now();
    impl.chisel->IntegrateDepthScan<float>(impl.integrator, impl.depth, ToExtrinsic(pose),
                                           impl.camera);
    const auto integrateEnded = std::chrono::steady_clock::now();

    // --- the one number that says where the frame went ---
    //
    // The frame loop already reports its own rate and the GPU already reports
    // its own milliseconds; between them sat an unattributed ~114 ms that was
    // known to be CPU and assumed to be here. Assumed is the word that made this
    // worth adding: the far plane below was chosen on the strength of that
    // assumption, and if it is wrong the number will say so instead of the
    // change quietly doing nothing.
    //
    // Reported as a mean over a window rather than per frame. Per frame it is
    // sixty lines a second in a log that is already unreadable, and the quantity
    // that matters is the steady state, not one sample of it.
    impl.integrateNanosTotal +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(integrateEnded - integrateBegan)
                    .count();
    ++impl.integrationsInWindow;
    if (impl.integrationsInWindow >= Impl::kTimingWindow) {
        const double meanMillis =
                static_cast<double>(impl.integrateNanosTotal) / impl.integrationsInWindow / 1e6;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "RECONPROBE integrate %.1f ms mean over %u frames; %u chunks held, "
                            "%u awaiting remesh; last frame %u fused at mean weight %.2f / "
                            "%u below confidence %u / %u out of [%.1f, %.1f] m, "
                            "furthest accepted %.2f m; %u frames skipped as empty",
                            meanMillis, impl.integrationsInWindow,
                            static_cast<uint32_t>(
                                    impl.chisel->GetChunkManager().GetChunks().size()),
                            pendingRemeshCount(),
                            validSamples,
                            validSamples > 0 ? weightSum / validSamples : 0.0,
                            lowConfidenceSamples,
                            static_cast<unsigned>(impl.config.confidenceThreshold),
                            rejectedSamples,
                            impl.config.nearPlane, impl.config.farPlane, observedMax,
                            impl.emptyFrames);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "RECONPROBE depth budget over the window: %.0f%% of samples had NO "
                            "estimate at all (%u of %u). Those are pixels ARCore's stereo could "
                            "not match -- untextured surfaces -- and no weighting reaches them.",
                            100.0 * impl.noEstimateSamples / std::max(impl.windowSamples, 1u),
                            impl.noEstimateSamples, impl.windowSamples);

        if (impl.histogramSamples > 0) {
            __android_log_print(
                    ANDROID_LOG_INFO, kLogTag,
                    "RECONPROBE confidence of the samples that DID exist, 32 wide: "
                    "%.0f%% %.0f%% %.0f%% %.0f%% %.0f%% %.0f%% %.0f%% %.0f%% "
                    "(gate is at %u)",
                    100.0 * impl.confidenceHistogram[0] / impl.histogramSamples,
                    100.0 * impl.confidenceHistogram[1] / impl.histogramSamples,
                    100.0 * impl.confidenceHistogram[2] / impl.histogramSamples,
                    100.0 * impl.confidenceHistogram[3] / impl.histogramSamples,
                    100.0 * impl.confidenceHistogram[4] / impl.histogramSamples,
                    100.0 * impl.confidenceHistogram[5] / impl.histogramSamples,
                    100.0 * impl.confidenceHistogram[6] / impl.histogramSamples,
                    100.0 * impl.confidenceHistogram[7] / impl.histogramSamples,
                    static_cast<unsigned>(impl.config.confidenceThreshold));
        }

        impl.integrateNanosTotal = 0;
        impl.integrationsInWindow = 0;
        impl.noEstimateSamples = 0;
        impl.histogramSamples = 0;
        impl.windowSamples = 0;
        impl.confidenceHistogram.fill(0);
    }

    if (!impl.loggedFirstIntegration) {
        impl.loggedFirstIntegration = true;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "RECONPROBE first integration done: %zu chunks, %zu awaiting remesh",
                            impl.chisel->GetChunkManager().GetChunks().size(),
                            impl.chisel->GetMeshesToUpdate().size());
    }

    if (!impl.loggedSparsity && validSamples > 0) {
        impl.loggedSparsity = true;
        const uint32_t totalSamples =
                static_cast<uint32_t>(depth.width) * static_cast<uint32_t>(depth.height);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "RECONPROBE first frame with data: %u of %u samples valid (%.0f%%), "
                            "%u below confidence %u/255 (map %s), "
                            "%u rejected as out of [%.2f, %.2f] m, furthest accepted %.2f m. "
                            "The frustum -- and so the number of chunks allocated -- is built "
                            "from the FURTHEST accepted sample, and grows with its cube.",
                            validSamples, totalSamples, 100.0 * validSamples / totalSamples,
                            lowConfidenceSamples,
                            static_cast<unsigned>(impl.config.confidenceThreshold),
                            useConfidence ? "present" : "ABSENT",
                            rejectedSamples, impl.config.nearPlane, impl.config.farPlane,
                            observedMax);
    }
}

uint32_t Reconstruction::Remesh(uint32_t maxChunks) {
    Impl& impl = *impl_;
    if (maxChunks == 0) {
        return 0;
    }

    chisel::ChunkSet& pending = impl.chisel->GetMutableMeshesToUpdate();
    chisel::ChunkManager& chunks = impl.chisel->GetMutableChunkManager();

    // --- fair draining, and why the obvious loop starves ---
    //
    // This used to walk `pending` from begin() every frame, taking the first
    // maxChunks it found and erasing them. That is a starvation bug, and it was
    // visible on the device as a reconstruction that meshed some regions
    // perfectly and never meshed others, with no geometric pattern to it -- an
    // object cut cleanly in half at a chunk boundary, the missing half never
    // arriving no matter how long the sensor looked at it.
    //
    // ChunkSet is an unordered_map, so begin() is the first non-empty HASH
    // BUCKET, which is a fixed and arbitrary function of the chunk coordinates.
    // Meanwhile integration re-marks the 3x3x3 neighbourhood of every chunk it
    // touched, so with the camera held still the same set is re-inserted every
    // frame, into the same buckets. The loop therefore drained the same
    // low-bucket chunks over and over and never reached the high-bucket ones.
    // The backlog sat pinned at 129 while 240 chunks a second were being
    // remeshed -- the budget was never the constraint, the ordering was.
    //
    // The fix is a cycle. Take EVERYTHING pending into a queue, drain the queue
    // at the budget over as many frames as it takes, and only refill once it is
    // empty. Chunks dirtied mid-cycle collect in `pending` and are picked up by
    // the next refill, so every dirty chunk is meshed within one cycle --
    // queueSize / maxChunks frames -- rather than never.
    if (impl.remeshQueue.empty() && !pending.empty()) {
        for (const auto& entry : pending) {
            impl.remeshQueue.push_back(entry.first);
        }
        pending.clear();
    }

    uint32_t done = 0;
    // A chunk that no longer exists is dropped rather than meshed: integration
    // marks a 3x3x3 neighbourhood, which reaches chunks that were never
    // allocated.
    while (!impl.remeshQueue.empty() && done < maxChunks) {
        const chisel::ChunkID id = impl.remeshQueue.front();
        impl.remeshQueue.pop_front();

        if (!chunks.HasChunk(id)) {
            continue;
        }
        chunks.RecomputeMesh(id, impl.meshMutex);
        ++done;

        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        const chisel::MeshMap& meshes = chunks.GetAllMeshes();
        const auto found = meshes.find(id);
        if (found != meshes.end() && found->second != nullptr) {
            vertexCount = static_cast<uint32_t>(found->second->vertices.size());
            indexCount = static_cast<uint32_t>(found->second->indices.size());
        }

        if (vertexCount == 0) {
            // Nothing to draw here. Reported as a removal only if it USED to
            // have something, so the renderer is not told to delete a node that
            // never existed.
            if (impl.live.erase(id) > 0) {
                impl.removed.push_back(ToChunkKey(id));
            }
            continue;
        }

        impl.live[id] = true;
        impl.changed.push_back(ChunkUpdate{ToChunkKey(id), impl.OriginOf(id), vertexCount,
                                           indexCount});
    }
    return done;
}

void Reconstruction::CollectUpdates(std::vector<ChunkUpdate>& changed,
                                    std::vector<ChunkKey>& removed) {
    changed.clear();
    removed.clear();
    changed.swap(impl_->changed);
    removed.swap(impl_->removed);
}

bool Reconstruction::WriteChunk(const ChunkKey& key, Vertex* vertices, uint32_t vertexCapacity,
                                uint32_t* indices, uint32_t indexCapacity) const {
    const Impl& impl = *impl_;
    if (vertices == nullptr || indices == nullptr) {
        return false;
    }

    const chisel::MeshMap& meshes = impl.chisel->GetChunkManager().GetAllMeshes();
    const auto found = meshes.find(ToChunkId(key));
    if (found == meshes.end() || found->second == nullptr) {
        return false;
    }
    const chisel::Mesh& mesh = *found->second;

    const auto vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    const auto indexCount = static_cast<uint32_t>(mesh.indices.size());
    if (vertexCount > vertexCapacity || indexCount > indexCapacity) {
        return false;
    }
    // Upstream asserts this rather than checking it, and our build defines
    // NDEBUG, so the assert is gone. Reading normals past their end would be a
    // silent out-of-bounds read into whatever follows the vector.
    if (mesh.normals.size() != mesh.vertices.size()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "RECONPROBE chunk %d,%d,%d has %zu vertices but %zu normals",
                            key.x, key.y, key.z, mesh.vertices.size(), mesh.normals.size());
        return false;
    }

    // The one place SoA becomes AoS, world becomes chunk-local, and size_t
    // becomes uint32. Writing straight into the caller's memory -- which may be
    // a mapped VkBuffer -- is why this takes a destination instead of returning
    // a span: it makes this the only pass over the data.
    const glm::vec3 origin = impl.OriginOf(ToChunkId(key));
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const chisel::Vec3& position = mesh.vertices[i];
        const chisel::Vec3& normal = mesh.normals[i];
        vertices[i].position =
                glm::vec3(position.x(), position.y(), position.z()) - origin;
        vertices[i].normal = glm::vec3(normal.x(), normal.y(), normal.z());
    }
    for (uint32_t i = 0; i < indexCount; ++i) {
        indices[i] = static_cast<uint32_t>(mesh.indices[i]);
    }
    return true;
}

void Reconstruction::MarkAllDirty() {
    Impl& impl = *impl_;
    impl.changed.clear();
    impl.removed.clear();
    impl.live.clear();
    // The cycle belongs to the old device's node tree. Everything is about to be
    // re-reported anyway, so carrying it over would only re-mesh chunks whose
    // geometry is already being handed back below.
    impl.remeshQueue.clear();

    for (const auto& entry : impl.chisel->GetChunkManager().GetAllMeshes()) {
        if (entry.second == nullptr || entry.second->vertices.empty()) {
            continue;
        }
        impl.live[entry.first] = true;
        impl.changed.push_back(ChunkUpdate{
                ToChunkKey(entry.first), impl.OriginOf(entry.first),
                static_cast<uint32_t>(entry.second->vertices.size()),
                static_cast<uint32_t>(entry.second->indices.size())});
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "RECONPROBE re-reporting %zu chunk meshes after a device rebuild",
                        impl.changed.size());
}

uint32_t Reconstruction::chunkCount() const {
    return static_cast<uint32_t>(impl_->chisel->GetChunkManager().GetChunks().size());
}

uint32_t Reconstruction::pendingRemeshCount() const {
    // Both halves: the cycle still draining and the marks that arrived while it
    // drained. Reporting only Chisel's set would read as zero at the exact
    // moment a full cycle has just been taken out of it.
    return static_cast<uint32_t>(impl_->chisel->GetMeshesToUpdate().size() +
                                 impl_->remeshQueue.size());
}

const Config& Reconstruction::config() const {
    return impl_->config;
}

namespace reconstruction_holder {

namespace {
std::unique_ptr<Reconstruction> g_reconstruction;
} // namespace

Reconstruction* Get() {
    return g_reconstruction.get();
}

Reconstruction* Create(const Config& config, std::string& error) {
    if (g_reconstruction == nullptr) {
        g_reconstruction = Reconstruction::Create(config, error);
    }
    return g_reconstruction.get();
}

void Destroy() {
    g_reconstruction.reset();
}

} // namespace reconstruction_holder

} // namespace putorana::recon
