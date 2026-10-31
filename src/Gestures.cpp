#include "Gestures.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kGraceSeconds = 0.15;   // keep a hand alive through brief tracking dropouts...
constexpr double kStrokeGraceSeconds = 0.4;   // ...longer mid-pinch, so a lost frame doesn't cut a stroke
constexpr double kPressLookback = 0.10;  // seconds: where the cursor was before the pinch closed
constexpr double kBeta = 0.02;           // One Euro speed coefficient (pixels/second units)
constexpr int kPinchFrames = 2;          // consecutive frames to confirm a pinch or tap
constexpr int kReleaseFrames = 4;        // ...and to confirm a pinch release: recorded strokes
                                         //    showed the thumb flickering away for 2-3 frames
constexpr float kFastSpeed = 2.5f;       // palm lengths per second that count as fast
constexpr float kFastReleaseSlack = 0.1f;   // a fast hand's pinch may open this much further
constexpr int kFistFrames = 3;           // fists are noisier, confirm over more frames
constexpr int kSettleFrames = 6;         // a new hand's frames before palm-toward-you counts
constexpr float kTapSlack = 0.04f;       // the middle tip is a little harder to meet exactly...
constexpr float kRingSlack = 0.08f;      // ...and the ring tip, half hidden behind it, more so
constexpr float kIndexMargin = 0.85f;    // an index pinch needs the thumb clearly nearer the index
constexpr float kTapMargin = 0.6f;       // a tap needs the thumb much nearer that fingertip

float Dist3(const cv::Point3f& a, const cv::Point3f& b)
{
    const cv::Point3f d = a - b;
    return std::sqrt(d.dot(d));
}

cv::Point2f Mid(const cv::Point3f& a, const cv::Point3f& b)
{
    return cv::Point2f((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

// Debounces a boolean: it only flips after enough consecutive disagreeing samples.
void Debounce(bool raw, bool& state, int& onCount, int& offCount, int onFrames, int offFrames)
{
    if (!state) {
        onCount = raw ? onCount + 1 : 0;
        if (onCount >= onFrames) {
            state = true;
            offCount = 0;
        }
    } else {
        offCount = raw ? 0 : offCount + 1;
        if (offCount >= offFrames) {
            state = false;
            onCount = 0;
        }
    }
}

} // namespace

std::vector<PointerInput> GestureEngine::Update(const TrackingFrame& frame, const wxSize& board)
{
    std::vector<PointerInput> result;
    if (frame.imageSize.width <= 0 || board.x <= 0 || board.y <= 0)
        return result;

    const double now = frame.time;
    const double minCutoff = 3.0 * std::pow(0.1, std::clamp(settings.smoothing, 0.0, 1.0));
    const double reach = std::clamp(settings.reach, 0.3, 1.0);
    const float pinchOn = float(0.16 + 0.22 * std::clamp(settings.pinchSensitivity, 0.0, 1.0));
    const float pinchOff = pinchOn + 0.12f;
    const double iw = frame.imageSize.width;
    const double ih = frame.imageSize.height;

    // The central `reach` part of the camera view spans the whole board, so you
    // don't have to stretch to the edge of the camera's field of view.
    auto toBoard = [&](double x, double y) {
        const double u = (x / iw - 0.5) / reach + 0.5;
        const double v = (y / ih - 0.5) / reach + 0.5;
        return wxPoint2DDouble(u * board.x, v * board.y);
    };

    for (const Hand& hand : frame.hands) {
        const auto& L = hand.lm;
        if (Dist3(L[WRIST], L[MIDDLE_MCP]) < 1.f)
            continue;
        Track& t = m_tracks[hand.id];
        t.lastSeen = now;

        // Finger distances relative to palm size, so they don't depend on how far
        // the hand is from the camera. They come from the metric 3D landmarks when
        // the model provides them: image positions are foreshortened when the hand
        // tilts, and fingers that only overlap on screen look pinched.
        const bool metric = HasWorld(hand.world);
        const auto& D = metric ? hand.world : hand.lm;
        const float palm = Dist3(D[WRIST], D[MIDDLE_MCP]);
        const float index = Dist3(D[THUMB_TIP], D[INDEX_TIP]) / palm;
        const float middle = Dist3(D[THUMB_TIP], D[MIDDLE_TIP]) / palm;
        const float ring = Dist3(D[THUMB_TIP], D[RING_TIP]) / palm;
        const float pinky = Dist3(D[THUMB_TIP], D[PINKY_TIP]) / palm;

        // Fist: every fingertip curled back closer to the wrist than its middle joint.
        auto curled = [&](int pip, int tip) {
            return Dist3(D[tip], D[WRIST]) < 0.9f * Dist3(D[pip], D[WRIST]);
        };
        const bool fistRaw = curled(INDEX_PIP, INDEX_TIP) && curled(MIDDLE_PIP, MIDDLE_TIP) &&
                             curled(RING_PIP, RING_TIP) && curled(PINKY_PIP, PINKY_TIP);

        // Palm direction, positive = palm toward the camera (the sign flips between left
        // and right hands). From the 3D landmarks it is the palm's true angle, so "palm
        // toward you" means the hand turned past ~120 degrees (released below ~105),
        // whatever the finger spread. The image-only fallback, the winding of wrist ->
        // index knuckle -> pinky knuckle, is weaker for narrow or tilted hands.
        float facing = 0.f, facingOn = -0.5f, facingOff = -0.25f;
        if (metric) {
            facing = PalmNormalZ(hand.world);
        } else {
            const cv::Point2f a(L[INDEX_MCP].x - L[WRIST].x, L[INDEX_MCP].y - L[WRIST].y);
            const cv::Point2f b(L[PINKY_MCP].x - L[WRIST].x, L[PINKY_MCP].y - L[WRIST].y);
            facing = (a.x * b.y - a.y * b.x) / std::max(1e-3f, float(cv::norm(a) * cv::norm(b)));
            facingOn = -0.30f;
            facingOff = -0.10f;
        }
        if (!hand.isRight) facing = -facing;
        t.palmScore = 0.6f * t.palmScore + 0.4f * facing;
        // The model's left/right guess can be wrong for a hand's first few frames (which
        // flips this sign), so a new hand can't count as palm-toward-you right away.
        ++t.frames;
        if (!t.pinch) {   // orientation is latched while a pinch is held
            if (!t.palmFacing && t.palmScore < facingOn && t.frames > kSettleFrames) t.palmFacing = true;
            else if (t.palmFacing && t.palmScore > facingOff) t.palmFacing = false;
        }

        // Cursor: the pinch point, blended with steadier joints so that closing the
        // pinch doesn't drag the cursor (Quest anchors its ray similarly).
        const cv::Point2f anchor = Mid(L[THUMB_TIP], L[INDEX_TIP]) * 0.6f +
                                   Mid(L[THUMB_IP], L[INDEX_MCP]) * 0.4f;
        const wxPoint2DDouble raw = toBoard(anchor.x, anchor.y);
        t.fx.SetParams(minCutoff, kBeta);
        t.fy.SetParams(minCutoff, kBeta);
        const wxPoint2DDouble pos(t.fx.Filter(raw.m_x, now), t.fy.Filter(raw.m_y, now));
        t.history.emplace_back(now, pos);
        while (t.history.size() > 10)
            t.history.pop_front();

        const cv::Point3f palmCenter = (L[WRIST] + L[INDEX_MCP] + L[MIDDLE_MCP] + L[PINKY_MCP]) * 0.25f;
        const wxPoint2DDouble palmRaw = toBoard(palmCenter.x, palmCenter.y);
        t.gx.SetParams(minCutoff, kBeta);
        t.gy.SetParams(minCutoff, kBeta);
        const wxPoint2DDouble grabPos(t.gx.Filter(palmRaw.m_x, now), t.gy.Filter(palmRaw.m_y, now));

        // Hand speed in palm lengths per second. Fast strokes blur the fingertips, and a
        // blurred frame can look like the pinch opening.
        const float palmPx = std::max(1.f, std::hypot(L[MIDDLE_MCP].x - L[WRIST].x, L[MIDDLE_MCP].y - L[WRIST].y));
        if (t.lastTime >= 0.0 && now > t.lastTime)
            t.speed = 0.5f * t.speed + 0.5f * float(cv::norm(anchor - t.lastAnchor) / palmPx / (now - t.lastTime));
        t.lastAnchor = anchor;
        t.lastTime = now;
        const bool fast = t.speed > kFastSpeed;

        Debounce(fistRaw && !t.pinch, t.fist, t.fistOn, t.fistOff, kFistFrames, kFistFrames);

        // The thumb pinches whichever fingertip it is nearer. The index finger rests
        // right beside the middle one and the thumb passes it on the way, so without
        // this a thumb + middle pinch keeps registering as an index pinch instead.
        // In a fist the thumb rests near the curled index tip; that is not a pinch.
        // A release needs the fingers open for several frames (wider while moving fast),
        // and the board trims the stroke back to where they began to open.
        const bool wasPinching = t.pinch;
        const float release = pinchOff + (fast ? kFastReleaseSlack : 0.f);
        const bool pinchRaw = t.pinch ? index < release
                                      : (index < pinchOn && index < kIndexMargin * std::min(middle, ring) && !fistRaw &&
                                         !t.fist && !t.middle.on && !t.ring.on && !t.pinky.on);
        Debounce(pinchRaw, t.pinch, t.pinchOn, t.pinchOff, kPinchFrames, kReleaseFrames);
        if (t.pinch && !wasPinching) {
            t.pinchIsSystem = t.palmFacing;
            t.pressPos = pos;
            for (const auto& [stamp, p] : t.history) {
                if (stamp <= now - kPressLookback) t.pressPos = p;
                else break;
            }
            if (t.history.front().first > now - kPressLookback)
                t.pressPos = t.history.front().second;
        }

        // Undo, the quick menu and clearing the board are deliberate thumb taps on the
        // middle, ring and pinky fingertips. Recorded hands showed false taps from
        // relaxed, bunched hands, where the thumb sits about as near the neighbouring
        // tips, often just as the hand came into view. So the thumb must first be apart
        // from that finger (arming the tap), then land much nearer it than any other
        // fingertip, none of them pinch-close.
        auto tapped = [&](Tap& tap, float d, float nearestOther, bool blocked, float slack, float margin) {
            if (d > pinchOff + slack)
                tap.armed = true;
            const bool raw = !t.fist && !fistRaw && !t.pinch && !blocked &&
                             (tap.on ? d < pinchOff + slack
                                     : tap.armed && d < pinchOn + slack && d < margin * nearestOther &&
                                           nearestOther > 0.9f * pinchOn);
            const bool was = tap.on;
            Debounce(raw, tap.on, tap.onCount, tap.offCount, kPinchFrames, kPinchFrames);
            if (!tap.on || was || now - tap.last <= 0.5)
                return false;
            tap.last = now;
            tap.armed = false;
            return true;
        };
        const bool middleTap =
            tapped(t.middle, middle, std::min(index, ring), t.ring.on || t.pinky.on, kTapSlack, kTapMargin);
        const bool ringTap =
            tapped(t.ring, ring, std::min({index, middle, pinky}), t.middle.on || t.pinky.on, kRingSlack, 0.7f);
        const bool pinkyTap =
            tapped(t.pinky, pinky, std::min({index, middle, ring}), t.middle.on || t.ring.on, kTapSlack, kTapMargin);

        const float closeness = std::clamp((0.95f - index) / (0.95f - pinchOn), 0.f, 1.f);
        t.strength = 0.5f * t.strength + 0.5f * closeness;

        PointerInput in;
        in.id = hand.id;
        in.isRight = hand.isRight;
        in.pos = pos;
        in.pressPos = t.pressPos;
        in.grabPos = grabPos;
        in.down = t.pinch && !t.pinchIsSystem;
        in.system = t.pinch && t.pinchIsSystem;
        in.releasing = t.pinch && index > release;
        in.grab = t.fist;
        in.palmFacing = t.palmFacing;
        in.middleTap = middleTap;
        in.ringTap = ringTap;
        in.pinkyTap = pinkyTap;
        in.strength = t.pinch ? 1.f : t.strength;
        // Skeleton, shifted so it lines up with the smoothed cursor.
        const double sx = pos.m_x - raw.m_x;
        const double sy = pos.m_y - raw.m_y;
        for (int i = 0; i < LANDMARK_COUNT; ++i) {
            const wxPoint2DDouble p = toBoard(L[i].x, L[i].y);
            in.skeleton[i] = wxPoint2DDouble(p.m_x + sx, p.m_y + sy);
        }
        in.hasSkeleton = true;
        t.last = in;
        result.push_back(in);
    }

    // Hands missing from this frame: hold their last state briefly (longer mid-stroke,
    // so the tracker can find the hand again under the same id), then drop them.
    for (auto it = m_tracks.begin(); it != m_tracks.end();) {
        Track& t = it->second;
        if (t.lastSeen == now) {
            ++it;
        } else if (now - t.lastSeen < (t.pinch ? kStrokeGraceSeconds : kGraceSeconds)) {
            PointerInput in = t.last;
            in.middleTap = false;
            in.ringTap = false;
            in.pinkyTap = false;
            result.push_back(in);
            ++it;
        } else {
            it = m_tracks.erase(it);
        }
    }
    return result;
}
