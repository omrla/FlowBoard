#pragma once

#include "HandTracker.h"
#include "OneEuroFilter.h"

#include <wx/gdicmn.h>
#include <wx/geometry.h>

#include <array>
#include <deque>
#include <map>
#include <utility>
#include <vector>

struct GestureSettings {
    double smoothing = 0.5;          // 0..1, higher = steadier but laggier cursor
    double reach = 0.7;              // fraction of the camera view mapped onto the board
    double pinchSensitivity = 0.5;   // 0..1, higher = pinch registers with fingers further apart
};

// One pointing device (a tracked hand or the mouse) in board-window pixels.
struct PointerInput {
    int id = 0;
    bool isMouse = false;
    bool isRight = true;
    wxPoint2DDouble pos;        // cursor (pinch point)
    wxPoint2DDouble pressPos;   // cursor just before the pinch closed (pinching nudges the hand)
    wxPoint2DDouble grabPos;    // palm centre, steadier while the fingers curl into a fist
    bool down = false;          // thumb + index pinch / left mouse button
    bool releasing = false;     // pinch is opening but the release isn't confirmed yet
    bool grab = false;          // fist / middle mouse button
    bool system = false;        // pinch with the palm facing you (recenters the view)
    bool palmFacing = false;    // palm turned toward you
    bool middleTap = false;     // thumb + middle finger tap started this frame (undo)
    bool ringTap = false;       // thumb + ring finger tap started this frame (quick menu)
    bool pinkyTap = false;      // thumb + pinky tap started this frame (clear the board)
    float strength = 0.f;       // 0..1 how close the pinch is to closing
    bool hasSkeleton = false;
    std::array<wxPoint2DDouble, LANDMARK_COUNT> skeleton;
};

// Turns tracked hand landmarks into pointer input with Quest-like gestures.
class GestureEngine {
public:
    GestureSettings settings;

    std::vector<PointerInput> Update(const TrackingFrame& frame, const wxSize& board);
    void Reset() { m_tracks.clear(); }

private:
    // A deliberate thumb tap on one fingertip (undo, quick menu, clear).
    struct Tap {
        bool on = false, armed = false;
        int onCount = 0, offCount = 0;
        double last = -10.0;
    };

    struct Track {
        OneEuroFilter fx, fy, gx, gy;
        std::deque<std::pair<double, wxPoint2DDouble>> history;
        wxPoint2DDouble pressPos;
        float strength = 0.f;
        float palmScore = 0.f;
        bool palmFacing = false;
        int frames = 0;           // frames this hand has been seen
        bool pinch = false, pinchIsSystem = false;
        int pinchOn = 0, pinchOff = 0;
        bool fist = false;
        int fistOn = 0, fistOff = 0;
        Tap middle, ring, pinky;
        cv::Point2f lastAnchor;   // image pixels, for the hand's speed
        double lastTime = -1.0;
        float speed = 0.f;        // palm lengths per second, smoothed
        double lastSeen = 0.0;
        PointerInput last;
    };

    std::map<int, Track> m_tracks;
};
