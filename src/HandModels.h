#pragma once

#include "NeuralNet.h"

#include <opencv2/core.hpp>

#include <array>
#include <filesystem>
#include <vector>

// The two models (ONNX exports from the OpenCV Zoo), in the app's models/ folder.
inline constexpr char kPalmModelFile[] = "palm_detection_mediapipe_2023feb.onnx";
inline constexpr char kHandModelFile[] = "handpose_estimation_mediapipe_2023feb.onnx";

// Palm detection result in image pixels. Keypoints are: wrist, index MCP,
// middle MCP, ring MCP, pinky MCP, thumb CMC, thumb MCP.
struct PalmDetection {
    cv::Rect2f box;
    std::array<cv::Point2f, 7> keypoints;
    float score = 0.f;
};

struct HandLandmarks {
    // Image pixels; z is depth relative to the wrist, in the same scale as x/y
    // (smaller = closer to the camera).
    std::array<cv::Point3f, 21> points;
    // Metric 3D positions in metres around the hand's centre, with the image's axes.
    // Unlike image positions they aren't foreshortened when the hand tilts, so
    // finger distances stay right however the hand is turned. All zero if the
    // model has no such output.
    std::array<cv::Point3f, 21> world{};
    float confidence = 0.f;
    float handedness = 0.f;   // > 0.5 means a right hand
};

// The square, rotated image region the landmark model looks at.
struct HandRoi {
    cv::Point2f center;       // image pixels
    float size = 0.f;         // side length in image pixels
    float rotation = 0.f;     // radians; turning the region by this makes the hand point up
};

// MediaPipe palm detector (192x192 SSD), ONNX export from OpenCV Zoo.
class PalmDetector {
public:
    static constexpr int kInputSize = 192;

    // Loads the model, on the graphics card when `gpu` is true and one is usable.
    void Load(const std::filesystem::path& modelPath, bool gpu = false);
    std::vector<PalmDetection> Detect(const cv::Mat& bgr, float scoreThreshold = 0.6f,
                                      float nmsThreshold = 0.3f);
    const NeuralNet& Net() const { return m_net; }

private:
    NeuralNet m_net;
    std::vector<cv::Point2f> m_anchors;
};

// MediaPipe hand landmark model (224x224), ONNX export from OpenCV Zoo.
class HandLandmarker {
public:
    static constexpr int kInputSize = 224;

    // Loads the model, on the graphics card when `gpu` is true and one is usable.
    void Load(const std::filesystem::path& modelPath, bool gpu = false);
    const NeuralNet& Net() const { return m_net; }

    // Runs the landmark model on the hand inside `roi`.
    bool Estimate(const cv::Mat& bgr, const HandRoi& roi, float minConfidence, HandLandmarks& out);

    // Region for a newly detected palm, enlarged to take in the fingers.
    static HandRoi RoiFromPalm(const PalmDetection& palm);

    // Region for the next frame from this frame's landmarks, so a tracked hand does
    // not need the (slower, jumpier) palm detector again.
    static HandRoi RoiFromLandmarks(const HandLandmarks& hand);

private:
    // Affine map from model-input pixels (224x224, hand upright) to image pixels.
    static cv::Matx23d CropTransform(const HandRoi& roi);

    NeuralNet m_net;
};
