#include "director/director.h"

#include "director/json.h"

#include <cstddef>
#include <string>

namespace br::director {

const char* module_name() noexcept { return "director"; }

namespace {

// Copy `s` into the fixed caption buffer keeping only printable ASCII (0x20..0x7E);
// other bytes are dropped (the HUD font is a 5x7 ASCII set). Truncates to the cap.
// Returns the kept length.
std::size_t sanitize_caption(const std::string& s, char (&out)[contracts::kDirectiveCaptionCap]) {
    std::size_t n = 0;
    for (char c : s) {
        if (n + 1 >= static_cast<std::size_t>(contracts::kDirectiveCaptionCap)) break;
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u <= 0x7E) out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

bool get_number(const json::Value& obj, const char* key, double& out) {
    const json::Value* v = obj.find(key);
    if (!v || !v->is_number()) return false;
    out = v->num;
    return true;
}

// Caption from "detail" (preferred) or "caption". False if neither is a string.
bool get_caption_raw(const json::Value& obj, std::string& out) {
    const json::Value* v = obj.find("detail");
    if (!v || !v->is_string()) v = obj.find("caption");
    if (!v || !v->is_string()) return false;
    out = v->str;
    return true;
}

float clamp01(double x) { return static_cast<float>(x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x)); }

}  // namespace

DirectiveResult validate_directive(const std::string& json_content) {
    DirectiveResult r;
    json::Value root;
    std::string perr;
    if (!json::parse(json_content, root, perr)) { r.reject_reason = "malformed JSON: " + perr; return r; }
    if (!root.is_object()) { r.reject_reason = "directive is not a JSON object"; return r; }

    const json::Value* type = root.find("type");
    if (!type || !type->is_string()) { r.reject_reason = "missing string field 'type'"; return r; }
    const std::string& t = type->str;

    contracts::Directive d{};
    std::string cap;

    if (t == "flicker") {
        d.kind = contracts::DirectiveKind::FlickerSector;
        double sec = 0.0;
        if (!get_number(root, "sector", sec)) { r.reject_reason = "flicker missing numeric 'sector'"; return r; }
        const int32_t s = static_cast<int32_t>(sec);
        if (s < 0 || s >= contracts::kDirectorMaxSector) { r.reject_reason = "flicker 'sector' out of range"; return r; }
        d.sector = s;
        double inten = 1.0; get_number(root, "intensity", inten);
        d.intensity = clamp01(inten);
        if (get_caption_raw(root, cap)) sanitize_caption(cap, d.caption);
    } else if (t == "sound") {
        d.kind = contracts::DirectiveKind::SoundCue;
        double inten = 1.0; get_number(root, "intensity", inten);
        d.intensity = clamp01(inten);
        if (get_caption_raw(root, cap)) sanitize_caption(cap, d.caption);
    } else if (t == "biome_bias") {
        d.kind = contracts::DirectiveKind::BiomeBias;
        double bi = 0.0;
        if (!get_number(root, "biome", bi)) { r.reject_reason = "biome_bias missing numeric 'biome'"; return r; }
        const int32_t b = static_cast<int32_t>(bi);
        if (b < 0 || b >= contracts::kDirectorBiomeCount) { r.reject_reason = "biome_bias 'biome' out of range"; return r; }
        d.biome = b;
        if (get_caption_raw(root, cap)) sanitize_caption(cap, d.caption);
    } else if (t == "intercom" || t == "note") {
        d.kind = (t == "intercom") ? contracts::DirectiveKind::Intercom : contracts::DirectiveKind::WandererNote;
        if (!get_caption_raw(root, cap)) { r.reject_reason = t + " missing string 'detail'"; return r; }
        if (sanitize_caption(cap, d.caption) == 0) { r.reject_reason = t + " 'detail' empty after sanitise"; return r; }
    } else {
        r.reject_reason = "unknown directive type '" + t + "'";
        return r;
    }

    r.ok = true;
    r.directive = d;
    return r;
}

}  // namespace br::director
