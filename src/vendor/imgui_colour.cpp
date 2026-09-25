// The overlay's colours, encoded for the swapchain the game presents to.
//
// ImGui's vertex colours are sRGB, and upstream's Vulkan backend writes them
// into the swapchain as they are. That is right for an SDR swapchain and
// wrong for the HDR10 one the game creates when the display is in HDR: the
// values are read as PQ, so dark greys go near black and mid-tones become
// saturated highlights. The draw lists are re-encoded once a frame, as they
// are snapshotted, from sRGB to what the swapchain expects. Textures other
// than the font atlas (icons) are not converted.

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../log.h"

namespace bg3le {
namespace {

enum class Encoding { Srgb, Linear, Pq };

std::atomic<Encoding> g_encoding{Encoding::Srgb};

float g_to_linear[256];
std::uint8_t g_pq[65536];      // linear BT.2020 in [0,1] of paper white -> PQ
std::uint8_t g_linear8[256];   // sRGB -> linear, eight bits

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float pq_encode(float nits) {
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float y = std::pow(std::fmax(nits, 0.0f) / 10000.0f, m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

// The game's own HDR paper white, so the overlay sits at the brightness of
// its interface. BT.2408's 203 nits if the setting is not there.
float paper_white() {
    char const* home = std::getenv("HOME");
    if (home == nullptr) return 203.0f;
    const std::string path = std::string(home)
        + "/.local/share/Larian Studios/Baldur's Gate 3/graphicSettings.lsx";
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return 203.0f;
    std::string text;
    char block[8192];
    std::size_t got = 0;
    while ((got = std::fread(block, 1, sizeof(block), f)) > 0) text.append(block, got);
    std::fclose(f);

    const std::size_t key = text.find("\"HDRPaperWhite\"");
    if (key == std::string::npos) return 203.0f;
    const std::size_t value = text.find("id=\"Value\"", key);
    const std::size_t quote = value == std::string::npos ? value : text.find("value=\"", value);
    if (quote == std::string::npos) return 203.0f;
    const float nits = std::strtof(text.c_str() + quote + 7, nullptr);
    return nits > 10.0f && nits < 1000.0f ? nits : 203.0f;
}

void build_tables(float paper) {
    for (int i = 0; i < 256; ++i) {
        g_to_linear[i] = srgb_to_linear(i / 255.0f);
        g_linear8[i] = (std::uint8_t)std::lround(g_to_linear[i] * 255.0f);
    }
    for (int i = 0; i < 65536; ++i) {
        g_pq[i] = (std::uint8_t)std::lround(pq_encode(i / 65535.0f * paper) * 255.0f);
    }
}

bool is_srgb_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

std::uint8_t pq_of(float linear) {
    const float clamped = linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
    return g_pq[(int)(clamped * 65535.0f + 0.5f)];
}

// sRGB BT.709 -> PQ BT.2020, alpha kept.
ImU32 to_pq(ImU32 c) {
    const float r = g_to_linear[(c >> IM_COL32_R_SHIFT) & 0xff];
    const float g = g_to_linear[(c >> IM_COL32_G_SHIFT) & 0xff];
    const float b = g_to_linear[(c >> IM_COL32_B_SHIFT) & 0xff];
    const float r2 = 0.6274f * r + 0.3293f * g + 0.0433f * b;
    const float g2 = 0.0691f * r + 0.9195f * g + 0.0114f * b;
    const float b2 = 0.0164f * r + 0.0880f * g + 0.8956f * b;
    return ((ImU32)pq_of(r2) << IM_COL32_R_SHIFT) | ((ImU32)pq_of(g2) << IM_COL32_G_SHIFT)
        | ((ImU32)pq_of(b2) << IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}

ImU32 to_linear(ImU32 c) {
    return ((ImU32)g_linear8[(c >> IM_COL32_R_SHIFT) & 0xff] << IM_COL32_R_SHIFT)
        | ((ImU32)g_linear8[(c >> IM_COL32_G_SHIFT) & 0xff] << IM_COL32_G_SHIFT)
        | ((ImU32)g_linear8[(c >> IM_COL32_B_SHIFT) & 0xff] << IM_COL32_B_SHIFT)
        | (c & IM_COL32_A_MASK);
}

}  // namespace

// From the backend's swapchain hook: how the presented image is encoded.
void imgui_swapchain_format(VkFormat format, VkColorSpaceKHR space) {
    Encoding encoding = Encoding::Srgb;
    if (space == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
        encoding = Encoding::Pq;
    } else if (space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT || is_srgb_format(format)) {
        // Linear values; an 8-bit colour cannot go past 1.0, so an scRGB
        // overlay sits at 80 nits rather than paper white.
        encoding = Encoding::Linear;
    }

    const float paper = paper_white();
    build_tables(paper);
    g_encoding.store(encoding);
    logf("imgui: swapchain format %d, colour space %d; overlay colours %s",
         (int)format, (int)space,
         encoding == Encoding::Pq ? "encoded as HDR10 PQ" :
         encoding == Encoding::Linear ? "linearised" : "left as sRGB");
    if (encoding == Encoding::Pq) logf("imgui: HDR paper white %.0f nits", paper);
}

// Re-encodes a snapshot's vertex colours for the swapchain. Called once per
// snapshot, on the draw lists the backend has just cloned.
void imgui_encode_colours(ImDrawData* data) {
    const Encoding encoding = g_encoding.load();
    if (encoding == Encoding::Srgb || data == nullptr) return;
    for (ImDrawList* list : data->CmdLists) {
        for (ImDrawVert& v : list->VtxBuffer) {
            v.col = encoding == Encoding::Pq ? to_pq(v.col) : to_linear(v.col);
        }
    }
}

}  // namespace bg3le
