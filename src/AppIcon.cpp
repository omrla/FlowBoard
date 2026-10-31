#include "AppIcon.h"

#include <wx/file.h>
#include <wx/icon.h>
#include <wx/mstream.h>

#include <string>
#include <vector>

namespace {

// The sizes in FlowBoard.ico: what Explorer and the taskbar ask for at the usual
// display scales.
const int kIcoSizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};

} // namespace

wxBitmapBundle AppIconBundle()
{
    return wxBitmapBundle::FromSVGResource("FLOWBOARD_SVG", wxSize(32, 32));
}

wxIconBundle AppIcons()
{
    wxIconBundle icons;
    const wxBitmapBundle bundle = AppIconBundle();
    if (!bundle.IsOk())
        return icons;
    for (int size : {16, 20, 24, 32, 40, 48, 64, 256}) {
        wxIcon icon;
        icon.CopyFromBitmap(bundle.GetBitmap(wxSize(size, size)));
        icons.AddIcon(icon);
    }
    return icons;
}

bool WriteAppIconFile(const wxString& path)
{
    const wxBitmapBundle bundle = AppIconBundle();
    if (!bundle.IsOk())
        return false;
    std::vector<std::string> images;
    for (int size : kIcoSizes) {
        wxMemoryOutputStream png;
        if (!bundle.GetBitmap(wxSize(size, size)).ConvertToImage().SaveFile(png, wxBITMAP_TYPE_PNG))
            return false;
        std::string data(png.GetLength(), '\0');
        png.CopyTo(data.data(), data.size());
        images.push_back(std::move(data));
    }

    // ICONDIR, one ICONDIRENTRY per image, then the images themselves as PNG, which
    // Windows has accepted in icons since Vista.
    std::string ico;
    auto u8 = [&ico](unsigned v) { ico += char(v & 0xFF); };
    auto u16 = [&](unsigned v) { u8(v); u8(v >> 8); };
    auto u32 = [&](size_t v) { u16(unsigned(v & 0xFFFF)); u16(unsigned(v >> 16)); };
    u16(0);   // reserved
    u16(1);   // type: icon
    u16(unsigned(images.size()));
    size_t offset = 6 + 16 * images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        const unsigned size = kIcoSizes[i] >= 256 ? 0 : unsigned(kIcoSizes[i]);   // 0 means 256
        u8(size);   // width
        u8(size);   // height
        u8(0);      // palette colours
        u8(0);      // reserved
        u16(1);     // colour planes
        u16(32);    // bits per pixel
        u32(images[i].size());
        u32(offset);
        offset += images[i].size();
    }
    for (const std::string& image : images)
        ico += image;

    wxFile file;
    return file.Create(path, true) && file.Write(ico.data(), ico.size()) == ico.size();
}
