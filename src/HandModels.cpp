#include "HandModels.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>

// Pre/post-processing follows OpenCV Zoo's reference implementations
// (models/palm_detection_mediapipe/mp_palmdet.py and
//  models/handpose_estimation_mediapipe/mp_handpose.py), except that the hand
// crop is a single affine warp, as in MediaPipe itself.

namespace {

constexpr int kPalmInput = PalmDetector::kInputSize;
constexpr int kHandInput = HandLandmarker::kInputSize;
constexpr double kPi = 3.14159265358979323846;

// SSD anchors of the MediaPipe palm model: strides 8,16,16,16 with two anchors
// per cell and layer; layers sharing a stride are merged (24*24*2 + 12*12*6 = 2016).
std::vector<cv::Point2f> MakePalmAnchors()
{
    const int strides[] = {8, 16, 16, 16};
    const int layers = 4;
    std::vector<cv::Point2f> anchors;
    int layer = 0;
    while (layer < layers) {
        int perCell = 0;
        int last = layer;
        while (last < layers && strides[last] == strides[layer]) {
            perCell += 2;
            ++last;
        }
        const int grid = (kPalmInput + strides[layer] - 1) / strides[layer];
        for (int y = 0; y < grid; ++y)
            for (int x = 0; x < grid; ++x)
                for (int k = 0; k < perCell; ++k)
                    anchors.emplace_back((x + 0.5f) / grid, (y + 0.5f) / grid);
        layer = last;
    }
    return anchors;
}

// Wraps an HxWx3 float image as the 1xHxWx3 (NHWC) blob these models expect.
cv::Mat ToNhwcBlob(const cv::Mat& rgbFloat)
{
    const cv::Mat src = rgbFloat.isContinuous() ? rgbFloat : rgbFloat.clone();
    const int dims[] = {1, src.rows, src.cols, 3};
    cv::Mat blob(4, dims, CV_32F);
    std::memcpy(blob.data, src.data, src.total() * src.elemSize());
    return blob;
}

float Sigmoid(float x)
{
    x = std::clamp(x, -80.f, 80.f);
    return 1.f / (1.f + std::exp(-x));
}

// Rotation that turns the direction from -> to straight up (MediaPipe's convention).
float UprightRotation(const cv::Point2f& from, const cv::Point2f& to)
{
    double radians = kPi / 2 - std::atan2(-(to.y - from.y), to.x - from.x);
    radians -= 2 * kPi * std::floor((radians + kPi) / (2 * kPi));
    return float(radians);
}

// Square region around `points` with the hand turned upright by `rotation`: their
// bounding box, moved toward the fingers by `shift` times its height, with the long
// side scaled by `scale`.
HandRoi RotatedRoi(const cv::Point2f* points, int count, float rotation, float scale, float shift)
{
    const float c = std::cos(rotation);
    const float s = std::sin(rotation);
    cv::Point2f lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
    for (int i = 0; i < count; ++i) {
        const cv::Point2f& p = points[i];
        const cv::Point2f u(c * p.x + s * p.y, -s * p.x + c * p.y);   // upright frame
        lo = cv::Point2f(std::min(lo.x, u.x), std::min(lo.y, u.y));
        hi = cv::Point2f(std::max(hi.x, u.x), std::max(hi.y, u.y));
    }
    const cv::Point2f mid((lo.x + hi.x) / 2, (lo.y + hi.y) / 2 - shift * (hi.y - lo.y));
    HandRoi roi;
    roi.center = cv::Point2f(c * mid.x - s * mid.y, s * mid.x + c * mid.y);
    roi.size = scale * std::max(hi.x - lo.x, hi.y - lo.y);
    roi.rotation = rotation;
    return roi;
}

} // namespace

// ---------------------------------------------------------------------------
// PalmDetector

void PalmDetector::Load(const std::filesystem::path& modelPath, bool gpu)
{
    m_net.Load(modelPath, gpu);
    m_anchors = MakePalmAnchors();
}

std::vector<PalmDetection> PalmDetector::Detect(const cv::Mat& bgr, float scoreThreshold,
                                                float nmsThreshold)
{
    // Letterbox to 192x192, keeping the aspect ratio. Area averaging, because plain
    // bilinear sampling of a 1080p frame at 1/10 scale aliases away small hands.
    const float ratio = std::min(float(kPalmInput) / bgr.cols, float(kPalmInput) / bgr.rows);
    const int rw = static_cast<int>(bgr.cols * ratio);
    const int rh = static_cast<int>(bgr.rows * ratio);
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(rw, rh), 0, 0, cv::INTER_AREA);
    const int padW = kPalmInput - rw;
    const int padH = kPalmInput - rh;
    const int left = padW / 2;
    const int top = padH / 2;
    cv::Mat input;
    cv::copyMakeBorder(resized, input, top, padH - top, left, padW - left,
                       cv::BORDER_CONSTANT, cv::Scalar::all(0));
    cv::cvtColor(input, input, cv::COLOR_BGR2RGB);
    input.convertTo(input, CV_32FC3, 1.0 / 255.0);

    const std::vector<cv::Mat> outs = m_net.Run(ToNhwcBlob(input));

    // Outputs: box/keypoint regressors [1,2016,18] and raw scores [1,2016,1].
    const cv::Mat* regressors = nullptr;
    const cv::Mat* scores = nullptr;
    for (const cv::Mat& o : outs) {
        if (o.total() == m_anchors.size() * 18) regressors = &o;
        else if (o.total() == m_anchors.size()) scores = &o;
    }
    if (!regressors || !scores)
        return {};

    const float* reg = regressors->ptr<float>();
    const float* raw = scores->ptr<float>();
    const float scale = float(std::max(bgr.cols, bgr.rows));
    const cv::Point2f bias(left / ratio, top / ratio);
    auto toImage = [&](float nx, float ny) {
        return cv::Point2f(nx * scale - bias.x, ny * scale - bias.y);
    };

    std::vector<PalmDetection> candidates;
    std::vector<cv::Rect2d> boxes;
    std::vector<float> confidences;
    for (size_t i = 0; i < m_anchors.size(); ++i) {
        const float score = Sigmoid(raw[i]);
        if (score < scoreThreshold)
            continue;
        const float* d = reg + i * 18;
        const cv::Point2f a = m_anchors[i];
        const float cx = d[0] / kPalmInput + a.x;
        const float cy = d[1] / kPalmInput + a.y;
        const float w = d[2] / kPalmInput;
        const float h = d[3] / kPalmInput;

        PalmDetection palm;
        const cv::Point2f p1 = toImage(cx - w / 2, cy - h / 2);
        const cv::Point2f p2 = toImage(cx + w / 2, cy + h / 2);
        palm.box = cv::Rect2f(p1, p2);
        for (int k = 0; k < 7; ++k)
            palm.keypoints[k] = toImage(d[4 + 2 * k] / kPalmInput + a.x,
                                        d[5 + 2 * k] / kPalmInput + a.y);
        palm.score = score;
        candidates.push_back(palm);
        boxes.emplace_back(palm.box.x, palm.box.y, palm.box.width, palm.box.height);
        confidences.push_back(score);
    }

    std::vector<int> keep;
    cv::dnn::NMSBoxes(boxes, confidences, scoreThreshold, nmsThreshold, keep);
    std::vector<PalmDetection> result;
    for (int k : keep)
        result.push_back(candidates[k]);
    return result;
}

// ---------------------------------------------------------------------------
// HandLandmarker

void HandLandmarker::Load(const std::filesystem::path& modelPath, bool gpu)
{
    m_net.Load(modelPath, gpu);
}

cv::Matx23d HandLandmarker::CropTransform(const HandRoi& roi)
{
    // input (u, v) -> image: center + R(rotation) * ((u, v) - 112) * pixels per input pixel
    const double k = roi.size / kHandInput;
    const double c = std::cos(roi.rotation) * k;
    const double s = std::sin(roi.rotation) * k;
    const double h = kHandInput / 2.0;
    return cv::Matx23d(c, -s, roi.center.x - h * (c - s),
                       s, c, roi.center.y - h * (s + c));
}

HandRoi HandLandmarker::RoiFromPalm(const PalmDetection& palm)
{
    // Turn wrist -> middle knuckle upright, then shift toward the fingers and enlarge
    // so the square covers the whole hand (same factors as OpenCV Zoo's MPHandPose).
    return RotatedRoi(palm.keypoints.data(), int(palm.keypoints.size()),
                      UprightRotation(palm.keypoints[0], palm.keypoints[2]), 3.0f, 0.4f);
}

HandRoi HandLandmarker::RoiFromLandmarks(const HandLandmarks& hand)
{
    // MediaPipe's own tracking region (hand_landmark_landmarks_to_roi): the box around
    // the wrist and the lower finger joints, which jitter less than the fingertips,
    // doubled to take in the whole hand and leave room to move before the next frame.
    static const int kStable[] = {0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};
    cv::Point2f points[std::size(kStable)];
    for (size_t i = 0; i < std::size(kStable); ++i)
        points[i] = cv::Point2f(hand.points[kStable[i]].x, hand.points[kStable[i]].y);
    auto xy = [&](int i) { return cv::Point2f(hand.points[i].x, hand.points[i].y); };
    const cv::Point2f knuckles = ((xy(5) + xy(13)) * 0.5f + xy(9)) * 0.5f;
    return RotatedRoi(points, int(std::size(kStable)), UprightRotation(xy(0), knuckles), 2.0f, 0.1f);
}

bool HandLandmarker::Estimate(const cv::Mat& bgr, const HandRoi& roi, float minConfidence,
                              HandLandmarks& out)
{
    if (roi.size < 16)
        return false;
    const cv::Matx23d M = CropTransform(roi);
    const double scale = roi.size / kHandInput;   // image pixels per input pixel

    // One warp from the camera frame straight into the upright 224x224 hand crop
    // (MediaPipe's approach); areas outside the frame are black. A hand near a 1080p
    // camera covers several image pixels per input pixel, where bilinear sampling
    // aliases fine detail into landmark jitter, so that region is first shrunk to
    // about one pixel per input pixel with area averaging.
    const cv::Mat* source = &bgr;
    cv::Mat shrunk;
    cv::Matx23d toSource = M;
    if (scale > 1.5) {
        const double half = 0.5 * roi.size * (std::abs(std::cos(roi.rotation)) + std::abs(std::sin(roi.rotation)));
        const cv::Rect box = cv::Rect(cvFloor(roi.center.x - half), cvFloor(roi.center.y - half),
                                      cvCeil(2 * half) + 2, cvCeil(2 * half) + 2) &
                             cv::Rect(0, 0, bgr.cols, bgr.rows);
        if (box.width >= 2 && box.height >= 2) {
            const cv::Size size(std::max(1, cvRound(box.width / scale)), std::max(1, cvRound(box.height / scale)));
            cv::resize(bgr(box), shrunk, size, 0, 0, cv::INTER_AREA);
            // Pixel centres line up as shrunk = (image - box corner + 0.5) * f - 0.5.
            const double fx = double(size.width) / box.width;
            const double fy = double(size.height) / box.height;
            toSource = cv::Matx23d(M(0, 0) * fx, M(0, 1) * fx, (M(0, 2) - box.x + 0.5) * fx - 0.5,
                                   M(1, 0) * fy, M(1, 1) * fy, (M(1, 2) - box.y + 0.5) * fy - 0.5);
            source = &shrunk;
        }
    }
    cv::Mat input;
    cv::warpAffine(*source, input, toSource, cv::Size(kHandInput, kHandInput),
                   cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, cv::Scalar::all(0));
    cv::cvtColor(input, input, cv::COLOR_BGR2RGB);
    input.convertTo(input, CV_32FC3, 1.0 / 255.0);

    const std::vector<cv::Mat> outs = m_net.Run(ToNhwcBlob(input));

    // Outputs (in order): landmarks [1,63], confidence [1,1], handedness [1,1],
    // world landmarks [1,63].
    const cv::Mat* landmarks = nullptr;
    const cv::Mat* world = nullptr;
    const cv::Mat* confidence = nullptr;
    const cv::Mat* handedness = nullptr;
    for (const cv::Mat& o : outs) {
        if (o.total() == 63) (landmarks ? world : landmarks) = &o;
        else if (o.total() == 1) (confidence ? handedness : confidence) = &o;
    }
    if (!landmarks || !confidence || !handedness)
        return false;
    if (world) {
        // Image-space landmarks are in input pixels (0..224); world ones are metres.
        double maxA = 0, maxB = 0;
        cv::minMaxIdx(cv::abs(*landmarks), nullptr, &maxA);
        cv::minMaxIdx(cv::abs(*world), nullptr, &maxB);
        if (maxA < maxB)
            std::swap(landmarks, world);
    }

    out.confidence = *confidence->ptr<float>();
    // The model scores "Left" assuming a mirrored (selfie) image. This is the camera's
    // own view, where that hand is the right one, so the score is that of a right hand.
    out.handedness = *handedness->ptr<float>();
    if (out.confidence < minConfidence)
        return false;

    // Landmarks are in input pixels: map them back with the same transform.
    const float* lm = landmarks->ptr<float>();
    for (int i = 0; i < 21; ++i) {
        const double u = lm[3 * i], v = lm[3 * i + 1];
        out.points[i] = cv::Point3f(float(M(0, 0) * u + M(0, 1) * v + M(0, 2)),
                                    float(M(1, 0) * u + M(1, 1) * v + M(1, 2)),
                                    float(lm[3 * i + 2] * scale));
    }
    // World landmarks are in the upright crop's orientation: turn them back.
    out.world = {};
    if (world) {
        const float c = std::cos(roi.rotation);
        const float s = std::sin(roi.rotation);
        const float* w = world->ptr<float>();
        for (int i = 0; i < 21; ++i)
            out.world[i] = cv::Point3f(c * w[3 * i] - s * w[3 * i + 1], s * w[3 * i] + c * w[3 * i + 1],
                                       w[3 * i + 2]);
    }
    return true;
}

