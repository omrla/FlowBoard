#pragma once

#include <wx/colour.h>
#include <wx/string.h>

#include <set>
#include <string>

// Writes a single-page vector PDF: filled rectangles and stroked polylines with
// round caps/joins, optionally translucent. Coordinates are PDF points, y up,
// unless changed with Transform().
class PdfWriter {
public:
    PdfWriter(double widthPt, double heightPt);

    void FillRect(double x, double y, double w, double h, const wxColour& colour);
    // Concatenates [a b c d e f] to the current transformation matrix.
    void Transform(double a, double b, double c, double d, double e, double f);

    // A stroke is BeginStroke, MoveTo, any number of LineTo, then EndStroke.
    // A single point draws a dot (a zero-length line with round caps).
    void BeginStroke(const wxColour& colour, double width);
    void MoveTo(double x, double y);
    void LineTo(double x, double y);
    void EndStroke();

    bool Save(const wxString& path) const;

private:
    void Num(double value);
    void Colour(const wxColour& colour, const char* op);

    double m_width, m_height;
    std::string m_content;
    std::set<int> m_alphas;   // translucency levels used, one ExtGState each
    bool m_translucent = false;
};
