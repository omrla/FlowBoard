#include "HandTracker.h"

#include "LandmarkSmoother.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdlib>

namespace {

constexpr float kTrackConfidence = 0.5f;    // keep following a hand above this
constexpr float kDetectConfidence = 0.7f;   // accept a newly detected hand above this
constexpr int kRedetectInterval = 5;        // frames between palm searches while tracking
constexpr double kRelinkSeconds = 0.5;      // a hand found again this soon near where a lost one
constexpr float kRelinkPalms = 2.5f;        //    was heading (in palm lengths) keeps its id

cv::Point2f PalmCenter(const HandLandmarks& lm)
{
    cv::Point2f c;
    for (int i : {WRIST, INDEX_MCP, MIDDLE_MCP, RING_MCP, PINKY_MCP})
        c += cv::Point2f(lm.points[i].x, lm.points[i].y) * 0.2f;
    return c;
}

float PalmLength(const HandLandmarks& lm)
{
    return std::hypot(lm.points[MIDDLE_MCP].x - lm.points[WRIST].x, lm.points[MIDDLE_MCP].y - lm.points[WRIST].y);
}

struct TrackedHand {
    int id = 0;
    HandLandmarks lm;          // raw model output; the next frame's crop is built from it
    float handedness = 0.5f;   // smoothed, so the label doesn't flicker
    cv::Point2f center;        // palm centre of `lm`, image pixels
    cv::Point2f velocity;      // pixels per second, smoothed
    double time = -1.0;        // when `lm` was captured
    LandmarkSmoother points, world;   // steadier copies for the gestures and the screen

    void Follow(const HandLandmarks& next, double now)
    {
        const cv::Point2f c = PalmCenter(next);
        if (time >= 0.0 && now > time)
            velocity = velocity * 0.5f + (c - center) * float(0.5 / (now - time));
        center = c;
        time = now;
        lm = next;
    }

    // The next crop: around these landmarks, moved along with the hand's motion and
    // widened by the move, so a fast hand stays inside the crop instead of slipping
    // out of it (losing it mid-stroke).
    HandRoi PredictRoi(double now) const
    {
        HandRoi roi = HandLandmarker::RoiFromLandmarks(lm);
        const cv::Point2f shift = velocity * float(std::clamp(now - time, 0.0, 0.1));
        roi.center += shift * 0.8f;
        roi.size *= 1.f + std::min(0.5f, float(cv::norm(shift)) / std::max(roi.size, 1.f));
        return roi;
    }
};

// A hand that just stopped being tracked; if it turns up again nearby, it keeps its id,
// so a stroke in progress carries on instead of being cut.
struct LostHand {
    int id;
    cv::Point2f center, velocity;
    float palm;
    float handedness;
    double time;
};

cv::Rect2f Bounds(const HandLandmarks& hand)
{
    cv::Point2f lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
    for (const cv::Point3f& p : hand.points) {
        lo = cv::Point2f(std::min(lo.x, p.x), std::min(lo.y, p.y));
        hi = cv::Point2f(std::max(hi.x, p.x), std::max(hi.y, p.y));
    }
    return cv::Rect2f(lo, hi);
}

float IoU(const cv::Rect2f& a, const cv::Rect2f& b)
{
    const float inter = (a & b).area();
    const float uni = a.area() + b.area() - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

cv::Mat MakePreview(const cv::Mat& frame, const std::vector<Hand>& hands)
{
    const int width = 320;
    const int height = frame.rows * width / frame.cols;
    cv::Mat preview;
    cv::resize(frame, preview, cv::Size(width, height), 0, 0, cv::INTER_AREA);
    cv::flip(preview, preview, 1);
    cv::cvtColor(preview, preview, cv::COLOR_BGR2RGB);

    const float s = float(width) / frame.cols;
    auto pt = [s](const cv::Point3f& p) { return cv::Point(cvRound(p.x * s), cvRound(p.y * s)); };
    for (const Hand& hand : hands) {
        const cv::Scalar bone = hand.isRight ? cv::Scalar(80, 160, 255) : cv::Scalar(255, 170, 60);
        for (const auto& b : kHandBones)
            cv::line(preview, pt(hand.lm[b[0]]), pt(hand.lm[b[1]]), bone, 2, cv::LINE_AA);
        for (const cv::Point3f& p : hand.lm)
            cv::circle(preview, pt(p), 2, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
    }
    return preview;
}

} // namespace

double HandTracker::Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

HandTracker::~HandTracker()
{
    Stop();
}

bool HandTracker::Start(int cameraIndex, const std::filesystem::path& modelDir, int maxHands, bool gpu)
{
    Stop();
    m_modelDir = modelDir;
    m_useGpu = gpu;
    m_maxHands = maxHands;
    m_stop = false;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_frame.release();
        m_frameSeq = 0;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latest = TrackingFrame();
    }
    SetState(TrackerState::Starting, "Opening camera " + std::to_string(cameraIndex) + "...");
    m_thread = std::thread(&HandTracker::Run, this, cameraIndex);
    return true;
}

bool HandTracker::LoadModels()
{
    if (m_loadedDir == m_modelDir && m_loadedGpu == m_useGpu)
        return true;
    SetState(TrackerState::Starting, m_useGpu ? "Preparing the hand models for the graphics card..."
                                              : "Loading the hand models...");
    try {
        m_loadedDir.clear();
        m_palm.Load(m_modelDir / kPalmModelFile, m_useGpu);
        m_landmarker.Load(m_modelDir / kHandModelFile, m_useGpu);
        // The first run on the graphics card compiles its shaders; do it now, not on
        // the first camera frame.
        const cv::Mat blank(720, 1280, CV_8UC3, cv::Scalar::all(0));
        m_palm.Detect(blank);
        HandLandmarks lm;
        m_landmarker.Estimate(blank, HandRoi{cv::Point2f(640, 360), 300.f, 0.f}, 0.f, lm);
    } catch (const std::exception& e) {
        SetState(TrackerState::Failed, std::string("Could not load the hand models: ") + e.what());
        return false;
    }
    m_loadedDir = m_modelDir;
    m_loadedGpu = m_useGpu;
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool gpu = m_palm.Net().OnGpu() && m_landmarker.Net().OnGpu();
    m_device = gpu ? "GPU" : "CPU";
    m_gpuError = m_landmarker.Net().GpuError().empty() ? m_palm.Net().GpuError() : m_landmarker.Net().GpuError();
    return true;
}

std::string HandTracker::Device() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_device;
}

std::string HandTracker::GpuError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_gpuError;
}

void HandTracker::Stop()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != TrackerState::Failed) {
        m_state = TrackerState::Stopped;
        m_status = "Camera off";
    }
}

TrackerState HandTracker::GetState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

std::string HandTracker::GetStatus() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

bool HandTracker::IsActive() const
{
    const TrackerState state = GetState();
    return state == TrackerState::Starting || state == TrackerState::Running;
}

void HandTracker::SetState(TrackerState state, const std::string& status)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = state;
    m_status = status;
}

bool HandTracker::PollFrame(TrackingFrame& out, uint64_t& lastSeq)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_latest.seq == lastSeq)
        return false;
    out = m_latest;
    lastSeq = m_latest.seq;
    return true;
}

void HandTracker::Run(int cameraIndex)
{
    // Loaded here rather than on the UI thread: preparing them for the GPU takes a moment.
    if (!LoadModels())
        return;
    const std::string name = "Camera " + std::to_string(cameraIndex);
    SetState(TrackerState::Starting, "Opening " + name + "...");   // loading the models changed it
    const double openStart = Now();
    auto elapsed = [&] { return std::to_string(int((Now() - openStart) * 1000)) + " ms"; };
    // Media Foundation takes the resolution at open time. DirectShow rebuilds its
    // capture graph for every property change (~3 s each), so it is only a fallback.
    _putenv_s("OPENCV_VIDEOIO_MSMF_ENABLE_HW_TRANSFORMS", "0");   // otherwise MSMF opens slowly
    const std::vector<int> params = {cv::CAP_PROP_FRAME_WIDTH, 1280, cv::CAP_PROP_FRAME_HEIGHT, 720};
    cv::VideoCapture cap;
    if (!cap.open(cameraIndex, cv::CAP_MSMF, params) && !cap.open(cameraIndex, cv::CAP_DSHOW)) {
        SetState(TrackerState::Failed, "Could not open " + name);
        return;
    }
    SetState(TrackerState::Running, name + " running (" + cap.getBackendName() + ", models on the " + Device() +
                                         ", ready in " + elapsed() + ")");

    // Media Foundation queues frames while nobody reads them, so after a slow tracking
    // frame the next few would already be up to ~90 ms old. This thread keeps reading
    // and hands the tracker only the newest frame.
    std::thread tracking(&HandTracker::Track, this);
    int failures = 0;
    cv::Mat frame;

    while (!m_stop) {
        if (!cap.read(frame) || frame.empty()) {
            if (++failures > 100) {
                SetState(TrackerState::Failed, "Camera stopped sending frames");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        failures = 0;
        {
            // Swap instead of copying: a frame the tracker hasn't taken yet comes back
            // here and is overwritten by the next read, so only the newest one waits.
            std::lock_guard<std::mutex> lock(m_frameMutex);
            std::swap(frame, m_frame);
            m_frameTime = Now();
            ++m_frameSeq;
        }
        m_frameReady.notify_one();
    }

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_stop = true;
    }
    m_frameReady.notify_one();
    tracking.join();
}

void HandTracker::Track()
{
    std::vector<TrackedHand> tracked;
    std::vector<LostHand> lost;
    int nextId = 1;
    uint64_t lastFrame = 0;
    uint64_t seq = 0;
    int sinceSearch = 0;
    double fpsStart = Now();
    int fpsFrames = 0;
    float fps = 0.f;
    cv::Mat frame;

    try {
        while (true) {
            double captured = 0.0;
            {
                std::unique_lock<std::mutex> lock(m_frameMutex);
                m_frameReady.wait(lock, [&] { return m_stop || m_frameSeq != lastFrame; });
                if (m_stop)
                    return;
                std::swap(frame, m_frame);
                lastFrame = m_frameSeq;
                captured = m_frameTime;
            }
            const double start = Now();
            const int maxHands = std::clamp(m_maxHands.load(), 1, 2);

            // 1) Follow known hands using last frame's landmarks as the region of interest.
            //    (One at a time: each network already uses every core, so running
            //    them side by side measured slower.)
            std::vector<TrackedHand> next;
            for (TrackedHand& t : tracked) {
                HandLandmarks lm;
                const bool found = m_landmarker.Estimate(frame, t.PredictRoi(captured), kTrackConfidence, lm);
                // Two tracks that converged onto the same hand: keep the older one.
                const bool duplicate = found && std::any_of(next.begin(), next.end(), [&](const TrackedHand& o) {
                    return IoU(Bounds(o.lm), Bounds(lm)) > 0.5f;
                });
                if (!found || duplicate) {
                    if (!duplicate)
                        lost.push_back({t.id, t.center, t.velocity, PalmLength(t.lm), t.handedness, t.time});
                    continue;
                }
                t.Follow(lm, captured);
                t.handedness = 0.8f * t.handedness + 0.2f * lm.handedness;
                next.push_back(std::move(t));
            }
            lost.erase(std::remove_if(lost.begin(), lost.end(),
                                      [&](const LostHand& l) { return captured - l.time > kRelinkSeconds; }),
                       lost.end());

            // 2) Look for new hands: every frame while none is tracked, straight away when
            //    one was just lost (mid-stroke, every frame counts), every few frames otherwise.
            const bool justLost = next.size() < tracked.size();
            if (int(next.size()) < maxHands && (next.empty() || justLost || ++sinceSearch >= kRedetectInterval)) {
                sinceSearch = 0;
                for (const PalmDetection& palm : m_palm.Detect(frame)) {
                    if (int(next.size()) >= maxHands)
                        break;
                    const cv::Point2f c(palm.box.x + palm.box.width / 2, palm.box.y + palm.box.height / 2);
                    const bool known = std::any_of(next.begin(), next.end(), [&](const TrackedHand& o) {
                        const cv::Rect2f b = Bounds(o.lm);
                        return b.contains(c) || IoU(b, palm.box) > 0.2f;
                    });
                    HandLandmarks lm;
                    if (known || !m_landmarker.Estimate(frame, HandLandmarker::RoiFromPalm(palm), kDetectConfidence, lm))
                        continue;
                    TrackedHand fresh;
                    fresh.id = nextId;
                    fresh.handedness = lm.handedness;
                    // The same hand found again where a lost one was heading: same id.
                    const cv::Point2f at = PalmCenter(lm);
                    const auto relink = std::find_if(lost.begin(), lost.end(), [&](const LostHand& l) {
                        const cv::Point2f expected = l.center + l.velocity * float(captured - l.time);
                        return cv::norm(at - expected) < kRelinkPalms * std::max(l.palm, PalmLength(lm));
                    });
                    if (relink != lost.end()) {
                        fresh.id = relink->id;
                        fresh.handedness = relink->handedness;
                        fresh.center = relink->center;
                        fresh.velocity = relink->velocity;
                        fresh.time = relink->time;
                        lost.erase(relink);
                    } else {
                        ++nextId;
                    }
                    fresh.Follow(lm, captured);
                    next.push_back(std::move(fresh));
                }
            }
            if (int(next.size()) > maxHands)
                next.resize(maxHands);
            tracked = std::move(next);

            // 3) Publish in the mirrored (selfie) view so moving right moves the cursor right.
            TrackingFrame out;
            out.seq = ++seq;
            out.time = captured;
            out.imageSize = frame.size();
            for (TrackedHand& t : tracked) {
                std::array<cv::Point3f, LANDMARK_COUNT> points = t.lm.points, world = t.lm.world;
                t.points.Apply(points, captured);
                t.world.Apply(world, captured);
                Hand hand;
                hand.id = t.id;
                hand.isRight = t.handedness > 0.5f;
                for (int i = 0; i < LANDMARK_COUNT; ++i) {
                    hand.lm[i] = cv::Point3f(float(frame.cols - 1) - points[i].x, points[i].y, points[i].z);
                    hand.world[i] = cv::Point3f(-world[i].x, world[i].y, world[i].z);
                }
                out.hands.push_back(hand);
            }
            out.preview = MakePreview(frame, out.hands);
            const double done = Now();
            out.inferenceMs = float((done - start) * 1000.0);

            ++fpsFrames;
            if (done - fpsStart >= 1.0) {
                fps = float(fpsFrames / (done - fpsStart));
                fpsFrames = 0;
                fpsStart = done;
            }
            out.fps = fps;

            std::lock_guard<std::mutex> lock(m_mutex);
            m_latest = std::move(out);
        }
    } catch (const std::exception& e) {
        SetState(TrackerState::Failed, std::string("Hand tracking stopped: ") + e.what());
        m_stop = true;   // ends the capture thread too
    }
}
