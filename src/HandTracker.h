#pragma once

#include "HandModels.h"

#include <opencv2/core.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// MediaPipe hand landmark indices.
enum HandLandmark {
    WRIST = 0,
    THUMB_CMC, THUMB_MCP, THUMB_IP, THUMB_TIP,
    INDEX_MCP, INDEX_PIP, INDEX_DIP, INDEX_TIP,
    MIDDLE_MCP, MIDDLE_PIP, MIDDLE_DIP, MIDDLE_TIP,
    RING_MCP, RING_PIP, RING_DIP, RING_TIP,
    PINKY_MCP, PINKY_PIP, PINKY_DIP, PINKY_TIP,
    LANDMARK_COUNT
};

// Landmark pairs that make up the hand skeleton.
inline constexpr int kHandBones[][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 4},
    {0, 5}, {5, 6}, {6, 7}, {7, 8},
    {5, 9}, {9, 10}, {10, 11}, {11, 12},
    {9, 13}, {13, 14}, {14, 15}, {15, 16},
    {13, 17}, {17, 18}, {18, 19}, {19, 20}, {0, 17},
};

struct Hand {
    int id = -1;           // stays the same while the hand remains tracked
    bool isRight = true;
    // Image pixels in the mirrored (selfie) view; z relative to the wrist.
    std::array<cv::Point3f, LANDMARK_COUNT> lm;
    // Metric 3D landmarks in metres, mirrored like `lm`; all zero when unavailable.
    std::array<cv::Point3f, LANDMARK_COUNT> world{};
};

// True when the model supplied metric 3D landmarks (they are all zero otherwise).
inline bool HasWorld(const std::array<cv::Point3f, LANDMARK_COUNT>& world)
{
    const cv::Point3f d = world[MIDDLE_MCP] - world[WRIST];
    return d.dot(d) > 0.02f * 0.02f;
}

// How squarely the palm or the back of the hand faces the camera, from the metric 3D
// landmarks: the camera-axis part of the palm's unit normal. +-1 face-on, 0 edge-on,
// and the same at the same angle whatever the finger spread. Same sign convention as
// the image winding of wrist -> index knuckle -> pinky knuckle (it flips between
// left and right hands).
inline float PalmNormalZ(const std::array<cv::Point3f, LANDMARK_COUNT>& world)
{
    const cv::Point3f n = (world[INDEX_MCP] - world[WRIST]).cross(world[PINKY_MCP] - world[WRIST]);
    const float length = std::sqrt(n.dot(n));
    return length > 1e-9f ? n.z / length : 0.f;
}

struct TrackingFrame {
    uint64_t seq = 0;
    double time = 0.0;     // HandTracker::Now() when the frame was captured
    cv::Size imageSize;
    std::vector<Hand> hands;
    cv::Mat preview;       // small mirrored RGB camera image with the skeleton drawn
    float fps = 0.f;          // frames tracked per second
    float inferenceMs = 0.f;  // time spent tracking this frame
};

enum class TrackerState { Stopped, Starting, Running, Failed };

// Captures the webcam on one thread and tracks up to two hands on another, always
// on the newest frame, so a slow frame never leaves a backlog of stale ones.
class HandTracker {
public:
    HandTracker() = default;
    ~HandTracker();
    HandTracker(const HandTracker&) = delete;
    HandTracker& operator=(const HandTracker&) = delete;

    // Starts the capture thread, which first loads the models from modelDir (on the
    // graphics card when `gpu` is true and one is usable) and then opens the camera.
    bool Start(int cameraIndex, const std::filesystem::path& modelDir, int maxHands, bool gpu = true);
    void Stop();

    TrackerState GetState() const;
    std::string GetStatus() const;
    bool IsActive() const;
    void SetMaxHands(int count) { m_maxHands = count; }
    // Where the hand models run: "GPU" or "CPU" (empty until they are loaded).
    std::string Device() const;
    // Why the GPU isn't used, when it was asked for but isn't.
    std::string GpuError() const;

    // Copies the newest frame into `out` if it is newer than `lastSeq`.
    bool PollFrame(TrackingFrame& out, uint64_t& lastSeq);

    // Monotonic clock in seconds, shared by tracking and UI timestamps.
    static double Now();

private:
    void Run(int cameraIndex);   // capture thread: owns the camera
    void Track();                // tracking thread: runs the models
    bool LoadModels();           // on the capture thread, before the camera opens
    void SetState(TrackerState state, const std::string& status);

    PalmDetector m_palm;
    HandLandmarker m_landmarker;
    std::filesystem::path m_modelDir, m_loadedDir;
    bool m_useGpu = true, m_loadedGpu = false;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_maxHands{2};

    // Newest camera frame, handed from the capture thread to the tracking thread.
    std::mutex m_frameMutex;
    std::condition_variable m_frameReady;
    cv::Mat m_frame;
    double m_frameTime = 0.0;
    uint64_t m_frameSeq = 0;

    mutable std::mutex m_mutex;
    TrackerState m_state = TrackerState::Stopped;
    std::string m_status = "Camera off";
    std::string m_device, m_gpuError;
    TrackingFrame m_latest;
};
