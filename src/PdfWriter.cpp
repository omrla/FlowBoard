#include "PdfWriter.h"

#include <wx/file.h>
#include <wx/mstream.h>
#include <wx/zstream.h>

#include <charconv>
#include <cstdio>
#include <vector>

namespace {

// Locale-independent: PDF numbers always use '.' as the decimal separator.
std::string Format(double value)
{
    char buf[32];
    const auto result = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, 2);
    return std::string(buf, result.ptr);
}

} // namespace

PdfWriter::PdfWriter(double widthPt, double heightPt)
    : m_width(widthPt), m_height(heightPt)
{
    m_content = "1 J 1 j\n";   // round caps and joins, like the on-screen strokes
}

void PdfWriter::Num(double value)
{
    m_content += Format(value);
    m_content += ' ';
}

void PdfWriter::Colour(const wxColour& colour, const char* op)
{
    Num(colour.Red() / 255.0);
    Num(colour.Green() / 255.0);
    Num(colour.Blue() / 255.0);
    m_content += op;
    m_content += '\n';
}

void PdfWriter::FillRect(double x, double y, double w, double h, const wxColour& colour)
{
    Colour(colour, "rg");
    Num(x);
    Num(y);
    Num(w);
    Num(h);
    m_content += "re f\n";
}

void PdfWriter::Transform(double a, double b, double c, double d, double e, double f)
{
    for (double v : {a, b, c, d, e, f})
        Num(v);
    m_content += "cm\n";
}

void PdfWriter::BeginStroke(const wxColour& colour, double width)
{
    m_translucent = colour.Alpha() < 255;
    if (m_translucent) {
        m_alphas.insert(colour.Alpha());
        m_content += "q /A" + std::to_string(colour.Alpha()) + " gs\n";
    }
    Colour(colour, "RG");
    Num(width);
    m_content += "w\n";
}

void PdfWriter::MoveTo(double x, double y)
{
    Num(x);
    Num(y);
    m_content += "m\n";
}

void PdfWriter::LineTo(double x, double y)
{
    Num(x);
    Num(y);
    m_content += "l\n";
}

void PdfWriter::EndStroke()
{
    m_content += m_translucent ? "S Q\n" : "S\n";
}

bool PdfWriter::Save(const wxString& path) const
{
    // Deflate the content stream (PDF /FlateDecode is a zlib stream).
    wxMemoryOutputStream deflated;
    {
        wxZlibOutputStream zlib(deflated, wxZ_BEST_COMPRESSION, wxZLIB_ZLIB);
        zlib.Write(m_content.data(), m_content.size());
        zlib.Close();
    }
    std::string stream(deflated.GetLength(), '\0');
    deflated.CopyTo(stream.data(), stream.size());

    std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets;
    auto object = [&](const std::string& body) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    std::string states;
    int next = 5;   // ExtGState objects follow catalog, pages, page and content
    for (int alpha : m_alphas)
        states += "/A" + std::to_string(alpha) + " " + std::to_string(next++) + " 0 R ";

    object("<< /Type /Catalog /Pages 2 0 R >>");
    object("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + Format(m_width) + " " + Format(m_height) +
           "] /Resources << /ExtGState << " + states + ">> >> /Contents 4 0 R >>");
    object("<< /Length " + std::to_string(stream.size()) + " /Filter /FlateDecode >>\nstream\n" +
           stream + "\nendstream");
    for (int alpha : m_alphas) {
        const std::string a = Format(alpha / 255.0);
        object("<< /Type /ExtGState /CA " + a + " /ca " + a + " >>");
    }

    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t offset : offsets) {
        char entry[24];
        std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offset);
        pdf += entry;
    }
    pdf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";

    wxFile file;
    return file.Create(path, true) && file.Write(pdf.data(), pdf.size()) == pdf.size();
}
