// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrocdr: CorelDRAW drawings through libcdr, drawn with plutovg. See nitrocdr.h.
//
// libcdr reports the drawing through librevenge::RVNGDrawingInterface (the calls the
// SVG / ODG generators get): pages, styles, paths, bitmaps (as BMP files) and text
// frames of lines and spans. The Painter below records that as a display list in
// points (y down); render() replays it with plutovg for any scale / offset, skipping
// whatever lies outside the target.
#include "nitrocdr.h"

#include <libcdr/libcdr.h>
#include <librevenge-stream/librevenge-stream.h>
#include <librevenge/librevenge.h>
#include <plutovg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#ifndef M_PI   // (not defined by mingw's math.h)
#define M_PI 3.14159265358979323846
#endif

namespace {

using librevenge::RVNGProperty;
using librevenge::RVNGPropertyList;
using librevenge::RVNGPropertyListVector;
using librevenge::RVNGString;

// every plutovg call that may touch fonts: the glyph caches have no lock of their own
// here (no C11 threads on macOS), and faces are shared between documents
std::mutex g_lock;

// --- properties --------------------------------------------------------------

double points(const RVNGProperty *p, double def = 0.0) {   // lengths: inch (default), point, twip
    if (!p) return def;
    switch (p->getUnit()) {
    case librevenge::RVNG_POINT: return p->getDouble();
    case librevenge::RVNG_TWIP: return p->getDouble() / 20.0;
    case librevenge::RVNG_PERCENT:
    case librevenge::RVNG_UNIT_ERROR: return def;
    default: return p->getDouble() * 72.0;   // inch, generic
    }
}
double number(const RVNGProperty *p, double def = 0.0) { return p ? p->getDouble() : def; }
bool is(const RVNGPropertyList &l, const char *key, const char *value) {
    return l[key] && l[key]->getStr() == value;
}

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};
Color color(const RVNGProperty *p, Color def = Color()) {   // "#rrggbb"
    if (!p) return def;
    RVNGString s = p->getStr();
    const char *c = s.cstr();
    if (*c == '#') c++;
    unsigned v = 0;
    if (sscanf(c, "%6x", &v) != 1) return def;
    Color o;
    o.r = ((v >> 16) & 255) / 255.0f;
    o.g = ((v >> 8) & 255) / 255.0f;
    o.b = (v & 255) / 255.0f;
    return o;
}

// --- display list --------------------------------------------------------------

struct Image {
    plutovg_surface_t *s = nullptr;
    ~Image() { plutovg_surface_destroy(s); }
};
// libcdr's own bitmaps: uncompressed 32-bit BMPs (B G R A, bottom-up), alpha all 0 when opaque;
// read here directly, as stb_image would (twice as fast as stb_image + plutovg's conversion)
static plutovg_surface_t *bmp32(const unsigned char *b, size_t n) {
    auto u32 = [b](size_t o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; };
    if (n < 54 || b[0] != 'B' || b[1] != 'M' || u32(14) != 40 || (b[28] | b[29] << 8) != 32 || u32(30) != 0) return nullptr;
    uint32_t off = u32(10), w = u32(18);
    int32_t h = (int32_t)u32(22);
    uint32_t ah = h < 0 ? 0u - (uint32_t)h : (uint32_t)h;
    if (!w || !ah || w > 65535 || ah > 65535 || off > n || (n - off) / 4 / w < ah) return nullptr;
    const unsigned char *px = b + off;
    size_t count = (size_t)w * ah;
    bool alpha = false;
    for (size_t i = 0; i < count && !alpha; i++) alpha = px[i * 4 + 3] != 0;
    plutovg_surface_t *s = plutovg_surface_create((int)w, (int)ah);
    if (!s) return nullptr;
    unsigned char *data = plutovg_surface_get_data(s);
    int stride = plutovg_surface_get_stride(s);
    for (uint32_t y = 0; y < ah; y++) {
        const unsigned char *src = px + (size_t)(h > 0 ? ah - 1 - y : y) * w * 4;
        uint32_t *dst = (uint32_t *)(data + (size_t)stride * y);
        for (uint32_t x = 0; x < w; x++, src += 4) {
            uint32_t a = alpha ? src[3] : 255, r = src[2], g = src[1], bl = src[0];
            if (a != 255) {   // (premultiplied, as plutovg_convert_rgba_to_argb)
                r = r * a / 255;
                g = g * a / 255;
                bl = bl * a / 255;
            }
            dst[x] = a ? a << 24 | r << 16 | g << 8 | bl : 0;
        }
    }
    return s;
}

std::shared_ptr<Image> image(const RVNGProperty *p) {   // a BMP (or other stb_image format) property
    const librevenge::RVNGBinaryData *d = p ? p->getBinaryData() : nullptr;
    if (!d || !d->size()) return nullptr;
    auto img = std::make_shared<Image>();
    img->s = bmp32(d->getDataBuffer(), d->size());
    if (!img->s) img->s = plutovg_surface_load_from_image_data(d->getDataBuffer(), (int)d->size());
    return img->s ? img : nullptr;
}

enum Fill { FILL_NONE, FILL_SOLID, FILL_LINEAR, FILL_RADIAL, FILL_BITMAP };

struct Style {
    Fill fill = FILL_NONE;
    Color fillColor;
    float opacity = 1;   // of the fill
    bool evenodd = false;
    std::vector<plutovg_gradient_stop_t> stops;
    float angle = 0;           // linear: degrees (librevenge / ODF)
    float cx = .5f, cy = .5f, r = 1;   // radial: in bounding box units
    std::shared_ptr<Image> tile;       // bitmap / pattern fill
    float tileW = 0, tileH = 0;        // points, or fractions of the bounding box (tileRel)
    bool tileRel = false;
    float refX = 0, refY = 0;          // tile offset (fractions of a tile)
    bool stroke = false;
    Color strokeColor;
    float strokeWidth = 0;   // 0: hairline
    plutovg_line_cap_t cap = PLUTOVG_LINE_CAP_BUTT;
    plutovg_line_join_t join = PLUTOVG_LINE_JOIN_MITER;
    std::vector<float> dashes;
};

struct Span {
    plutovg_font_face_t *face = nullptr;   // owned by the font cache
    float size = 12;
    float scaleX = 1;   // horizontal stretch (CorelDRAW text squeezed to fit, style:text-scale)
    float shiftX = 0, shiftY = 0;   // moved from its place, in em (up positive; libcdr:shift-x / -y)
    Color color;
    std::string text;
    float width = 0;
    std::string font;           // the font asked for (and its style), for characters its face lacks
    bool bold = false, italic = false;
};
struct Line {
    int align = 0;   // 0 left, 1 center, 2 right
    std::vector<Span> spans;
    float width = 0, size = 0, ascent = 0;
    float height = 0;   // its line spacing: the font's ascent + descent (CorelDRAW's 100%)
    plutovg_font_face_t *emptyFace = nullptr;   // the font of the line break that started it (for an
    float emptySize = 0;                        // empty line: its height)
};
struct Text {
    float x = 0, y = 0, w = 0, h = 0, rotate = 0;   // frame (w = h = 0: artistic text at x, y)
    std::vector<Line> lines;
};

struct Item {
    enum Kind { PATH, IMAGE, TEXT } kind = PATH;
    std::shared_ptr<Style> style;
    std::unique_ptr<plutovg_path_t, void (*)(plutovg_path_t *)> path{nullptr, plutovg_path_destroy};
    std::shared_ptr<Image> img;
    float x = 0, y = 0, w = 0, h = 0, rotate = 0;   // image rectangle (rotated about its centre)
    bool mirrorX = false, mirrorY = false;
    std::unique_ptr<Text> text;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;   // bounding box (culling)
};

struct Page {
    double x = 0, y = 0, w = 0, h = 0;   // shown area: the drawing's bounding box (see nitrocdr_load)
    std::vector<Item> items;
};

// --- fonts -------------------------------------------------------------------

plutovg_font_face_cache_t *font_cache() {   // locked by the caller
    static plutovg_font_face_cache_t *cache;
    if (!cache) {
        cache = plutovg_font_face_cache_create();
        plutovg_font_face_cache_load_sys(cache);
    }
    return cache;
}

bool ends_with(const std::string &s, const char *t) {
    size_t n = strlen(t);
    return s.size() >= n && !strcasecmp(s.c_str() + s.size() - n, t);
}

// Bitstream's names of old CorelDRAW fonts: "Futura Lt BT", "AvantGarde Bk BT", "Futura XBlkCnIt BT":
// the family and the weight / italic of the abbreviations in its last word (width ones dropped)
static bool bitstream_name(std::string &family, int &weight, bool &italic) {
    if (!ends_with(family, " BT")) return false;
    family.resize(family.size() - 3);
    size_t sp = family.rfind(' ');
    if (sp == std::string::npos) return true;
    static const struct { const char *abbr; int weight; } abbrs[] = {   // weight -1: italic, 0: width
        {"XBlk", 900}, {"Blk", 900}, {"XBd", 800}, {"Bd", 700}, {"Hv", 800}, {"It", -1}, {"XLt", 200}, {"Lt", 300},
        {"Bk", 400}, {"Md", 500}, {"XCn", 0}, {"Cn", 0}, {"Ex", 0}};
    int w = weight;
    bool it = italic;
    for (size_t i = sp + 1; i < family.size();) {
        bool found = false;
        for (const auto &a : abbrs) {
            size_t n = strlen(a.abbr);
            if (!family.compare(i, n, a.abbr)) {
                if (a.weight > 0) w = std::max(a.weight, weight > 400 ? weight : 0);   // (bold stays bold)
                if (a.weight < 0) it = true;
                i += n;
                found = true;
                break;
            }
        }
        if (!found) return true;   // (an ordinary word)
    }
    family.resize(sp);
    weight = w;
    italic = it;
    return true;
}

// Families of old CorelDRAW drawings that macOS does not have: the closest one it has; and the
// Windows / macOS core fonts that Linux has only as their metric-compatible Liberation fonts.
static const char *substitute(const std::string &family) {
    static const char *const subst[][2] = {{"AvantGarde", "Avenir"}, {"Century Gothic", "Avenir"},
                                           {"Arial", "Liberation Sans"}, {"Helvetica", "Liberation Sans"},
                                           {"Times New Roman", "Liberation Serif"}, {"Times", "Liberation Serif"},
                                           {"Courier New", "Liberation Mono"}, {"Courier", "Liberation Mono"}};
    for (const auto &e : subst)
        if (!strcasecmp(family.c_str(), e[0])) return e[1];
    return nullptr;
}

// CorelDRAW font names: "Arial", "Arial CE" (Central European variants of old fonts),
// "Times New Roman Bold", "Futura Lt BT" ... -> a face of the system fonts, falling back to
// Arial / Helvetica (Linux: Liberation Sans / DejaVu Sans).
plutovg_font_face_t *find_font(const std::string &name, bool bold, bool italic, std::string *familyOut = nullptr) {
    plutovg_font_face_cache_t *cache = font_cache();
    std::string family = name;
    int weight = bold ? 700 : 400;
    bool bitstream = bitstream_name(family, weight, italic);
    for (bool changed = true; changed;) {
        changed = false;
        static const char *const suffixes[] = {" CE", " Cyr", " Baltic", " Greek", " Tur"};
        for (const char *sfx : suffixes)
            if (ends_with(family, sfx)) { family.resize(family.size() - strlen(sfx)); changed = true; }
        static const char *const bolds[] = {" Bold", " Black", " Heavy", " Semibold"};
        for (const char *sfx : bolds)
            if (ends_with(family, sfx)) { family.resize(family.size() - strlen(sfx)); weight = std::max(weight, 700); changed = true; }
        static const char *const italics[] = {" Italic", " Oblique"};
        for (const char *sfx : italics)
            if (ends_with(family, sfx)) { family.resize(family.size() - strlen(sfx)); italic = changed = true; }
    }
    if (familyOut) *familyOut = family;
    const char *sub = substitute(family);
    // a Bitstream font also under Linotype's name of the family ("Futura Lt BT": "Futura LT" Light)
    std::string linotype = bitstream ? family + " LT" : family;
    const char *tries[] = {name.c_str(), linotype.c_str(), family.c_str(), sub ? sub : family.c_str(),
                           "Arial", "Helvetica", "Liberation Sans", "DejaVu Sans"};   // (from [4]: the last resorts)
    for (const char *t : tries) {
        // upright text: not from a family that has only italic faces (an installed "Futura Lt BT"
        // Light Italic alone made every Futura text italic); the next candidate instead
        if (!italic && t != tries[4] && t != tries[5] && t != tries[6] && t != tries[7] &&
            !plutovg_font_face_cache_has_style(cache, t, false) && plutovg_font_face_cache_has_style(cache, t, true))
            continue;
        plutovg_font_face_t *f = plutovg_font_face_cache_get_weight(cache, t, weight, italic);
        if (f) return f;
    }
    return nullptr;   // (a family without that style gives its closest face)
}

// A face for a character the font lacks (old Western fonts such as AvantGarde Bk BT have no ő / ű:
// their "missing glyph" box showed): the family's substitute, then common fonts; null if none has it.
plutovg_font_face_t *fallback_font(const std::string &name, bool bold, bool italic, plutovg_codepoint_t cp) {
    std::string family;
    find_font(name, bold, italic, &family);
    const char *sub = substitute(family);
    const char *tries[] = {sub ? sub : "Arial", "Arial", "Helvetica", "Lucida Grande", "Arial Unicode MS", "Liberation Sans",
                           "DejaVu Sans"};
    for (const char *t : tries) {
        plutovg_font_face_t *f = find_font(t, bold, italic);
        if (f && plutovg_font_face_has_glyph(f, cp)) return f;
    }
    return nullptr;
}

static plutovg_codepoint_t utf8_next(const std::string &s, size_t &i) {   // (invalid bytes: as Latin-1)
    unsigned char c = (unsigned char)s[i++];
    int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    plutovg_codepoint_t cp = n ? c & (0x3f >> n) : c;
    for (; n && i < s.size() && ((unsigned char)s[i] & 0xc0) == 0x80; n--) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3f);
    return cp;
}

// --- the painter -------------------------------------------------------------

class Painter : public librevenge::RVNGDrawingInterface {
public:
    std::vector<Page> pages;

    void startDocument(const RVNGPropertyList &) override {}
    void endDocument() override {}
    void setDocumentMetaData(const RVNGPropertyList &) override {}
    void defineEmbeddedFont(const RVNGPropertyList &) override {}
    void startPage(const RVNGPropertyList &p) override {
        pages.emplace_back();
        pages.back().w = points(p["svg:width"], 595.0);
        pages.back().h = points(p["svg:height"], 842.0);
        m_inPage = true;
    }
    void endPage() override { m_inPage = false; }
    void startMasterPage(const RVNGPropertyList &) override { m_inMaster = true; }
    void endMasterPage() override { m_inMaster = false; }
    void setStyle(const RVNGPropertyList &p) override { m_style = parseStyle(p); }
    void startLayer(const RVNGPropertyList &) override {}
    void endLayer() override {}
    void startEmbeddedGraphics(const RVNGPropertyList &) override {}
    void endEmbeddedGraphics() override {}
    void openGroup(const RVNGPropertyList &) override {}
    void closeGroup() override {}

    void drawRectangle(const RVNGPropertyList &p) override {
        plutovg_path_t *path = plutovg_path_create();
        double x = points(p["svg:x"]), y = points(p["svg:y"]), w = points(p["svg:width"]), h = points(p["svg:height"]);
        double rx = points(p["svg:rx"]), ry = points(p["svg:ry"]);
        if (rx > 0 || ry > 0) plutovg_path_add_round_rect(path, (float)x, (float)y, (float)w, (float)h, (float)rx, (float)ry);
        else plutovg_path_add_rect(path, (float)x, (float)y, (float)w, (float)h);
        addPath(path);
    }
    void drawEllipse(const RVNGPropertyList &p) override {
        plutovg_path_t *path = plutovg_path_create();
        double cx = points(p["svg:cx"]), cy = points(p["svg:cy"]), rx = points(p["svg:rx"]), ry = points(p["svg:ry"]);
        plutovg_path_add_ellipse(path, (float)cx, (float)cy, (float)rx, (float)ry);
        double rot = number(p["librevenge:rotate"]);
        if (rot != 0) {
            plutovg_matrix_t m;
            plutovg_matrix_init_translate(&m, (float)cx, (float)cy);
            plutovg_matrix_rotate(&m, (float)(-rot * M_PI / 180));
            plutovg_matrix_translate(&m, (float)-cx, (float)-cy);
            plutovg_path_transform(path, &m);
        }
        addPath(path);
    }
    void drawPolygon(const RVNGPropertyList &p) override { drawPoints(p, true); }
    void drawPolyline(const RVNGPropertyList &p) override { drawPoints(p, false); }
    void drawPath(const RVNGPropertyList &p) override {
        const RVNGPropertyListVector *d = p.child("svg:d");
        if (!d || !d->count()) return;
        addPath(buildPath(*d));
    }
    // a librevenge path (svg:d)
    static plutovg_path_t *buildPath(const RVNGPropertyListVector &dv) {
        const RVNGPropertyListVector *d = &dv;
        plutovg_path_t *path = plutovg_path_create();
        for (unsigned long i = 0; i < d->count(); i++) {
            const RVNGPropertyList &e = (*d)[i];
            if (!e["librevenge:path-action"]) continue;
            RVNGString a = e["librevenge:path-action"]->getStr();
            float x = (float)points(e["svg:x"]), y = (float)points(e["svg:y"]);
            switch (a.cstr()[0]) {
            case 'M': plutovg_path_move_to(path, x, y); break;
            case 'L': plutovg_path_line_to(path, x, y); break;
            case 'H': plutovg_path_line_to(path, x, currentY(path)); break;
            case 'V': plutovg_path_line_to(path, currentX(path), y); break;
            case 'C':
                plutovg_path_cubic_to(path, (float)points(e["svg:x1"]), (float)points(e["svg:y1"]), (float)points(e["svg:x2"]),
                                      (float)points(e["svg:y2"]), x, y);
                break;
            case 'Q': plutovg_path_quad_to(path, (float)points(e["svg:x1"]), (float)points(e["svg:y1"]), x, y); break;
            case 'A':
                plutovg_path_arc_to(path, (float)points(e["svg:rx"]), (float)points(e["svg:ry"]),
                                    (float)(number(e["librevenge:rotate"]) * M_PI / 180), e["librevenge:large-arc"] && e["librevenge:large-arc"]->getInt(),
                                    e["librevenge:sweep"] && e["librevenge:sweep"]->getInt(), x, y);
                break;
            case 'Z': plutovg_path_close(path); break;
            default: break;
            }
        }
        return path;
    }
    void drawGraphicObject(const RVNGPropertyList &p) override {
        if (!m_inPage || m_inMaster) return;
        std::shared_ptr<Image> img = cachedImage(p["office:binary-data"]);
        if (!img) return;
        Item it;
        it.kind = Item::IMAGE;
        it.style = m_style;
        it.img = img;
        it.x = (float)points(p["svg:x"]);
        it.y = (float)points(p["svg:y"]);
        it.w = (float)points(p["svg:width"]);
        it.h = (float)points(p["svg:height"]);
        it.rotate = (float)number(p["librevenge:rotate"]);
        it.mirrorX = p["draw:mirror-horizontal"] && p["draw:mirror-horizontal"]->getInt();
        it.mirrorY = p["draw:mirror-vertical"] && p["draw:mirror-vertical"]->getInt();
        float cx = it.x + it.w / 2, cy = it.y + it.h / 2;
        rotatedBox(it, cx, cy, it.x, it.y, it.x + it.w, it.y + it.h, it.rotate);
        // the bitmap's outline (libcdr:clip-path): it shows only inside (cropped bitmaps, the
        // pattern tiles of imported PDF plans); the clip, in page coordinates, goes in it.path
        if (const RVNGPropertyListVector *clip = p.child("libcdr:clip-path")) {
            it.path.reset(buildPath(*clip));
            plutovg_rect_t r;
            plutovg_path_extents(it.path.get(), &r, false);
            it.bx0 = std::max(it.bx0, r.x);
            it.by0 = std::max(it.by0, r.y);
            it.bx1 = std::min(it.bx1, r.x + r.w);
            it.by1 = std::min(it.by1, r.y + r.h);
            if (it.bx1 <= it.bx0 || it.by1 <= it.by0) return;   // (nothing of it shows)
        }
        add(std::move(it));
    }
    void drawConnector(const RVNGPropertyList &) override {}

    void startTextObject(const RVNGPropertyList &p) override {
        m_text.reset(new Text);
        m_text->x = (float)points(p["svg:x"]);
        m_text->y = (float)points(p["svg:y"]);
        m_text->w = (float)points(p["svg:width"]);
        m_text->h = (float)points(p["svg:height"]);
        m_text->rotate = (float)number(p["librevenge:rotate"]);
    }
    void endTextObject() override {
        if (!m_text) return;
        std::unique_ptr<Text> t = std::move(m_text);
        while (!t->lines.empty() && t->lines.back().spans.empty()) t->lines.pop_back();
        if (t->lines.empty() || !m_inPage || m_inMaster) return;
        // only spaces: nothing to draw (left-over texts far off the drawing would stretch its bounding box)
        bool blank = true;
        for (const Line &l : t->lines)
            for (const Span &sp : l.spans)
                if (sp.text.find_first_not_of(" \t\xC2\xA0") != std::string::npos) blank = false;
        if (blank) return;
        Item it;
        it.kind = Item::TEXT;
        it.text = std::move(t);
        layout(it);
        add(std::move(it));
    }
    void startTableObject(const RVNGPropertyList &) override {}
    void openTableRow(const RVNGPropertyList &) override {}
    void closeTableRow() override {}
    void openTableCell(const RVNGPropertyList &) override {}
    void closeTableCell() override {}
    void insertCoveredTableCell(const RVNGPropertyList &) override {}
    void endTableObject() override {}
    void insertTab() override { insertText(RVNGString("    ")); }
    void insertSpace() override { insertText(RVNGString(" ")); }
    void insertText(const RVNGString &s) override {
        if (!m_text) return;
        // line breaks inside the text (a CorelDRAW soft return) start a new line, same alignment
        const char *c = s.cstr();
        const char *start = c;
        for (;; c++) {
            if (*c == '\n' || *c == '\r' || *c == '\v' || !*c) {
                if (c > start) appendText(std::string(start, c - start));
                if (!*c) break;
                if (*c == '\r' && c[1] == '\n') c++;
                newLine();
                start = c + 1;
            }
        }
    }
    void insertLineBreak() override { newLine(); }
    void insertField(const RVNGPropertyList &) override {}
    void openOrderedListLevel(const RVNGPropertyList &) override {}
    void openUnorderedListLevel(const RVNGPropertyList &) override {}
    void closeOrderedListLevel() override {}
    void closeUnorderedListLevel() override {}
    void openListElement(const RVNGPropertyList &) override {}
    void closeListElement() override {}
    void defineParagraphStyle(const RVNGPropertyList &) override {}
    void openParagraph(const RVNGPropertyList &p) override {
        if (!m_text) return;
        m_align = 0;
        if (is(p, "fo:text-align", "center")) m_align = 1;
        else if (is(p, "fo:text-align", "end") || is(p, "fo:text-align", "right")) m_align = 2;
        m_text->lines.emplace_back();
        m_text->lines.back().align = m_align;
    }
    void closeParagraph() override {}
    void defineCharacterStyle(const RVNGPropertyList &) override {}
    void openSpan(const RVNGPropertyList &p) override {
        m_span = Span();
        m_span.size = (float)points(p["fo:font-size"], 12.0);
        double sx = number(p["style:text-scale"], 1.0);
        if (sx > 0.01 && sx < 100) m_span.scaleX = (float)sx;
        m_span.shiftX = (float)number(p["libcdr:shift-x"]);
        m_span.shiftY = (float)number(p["libcdr:shift-y"]);
        m_span.color = color(p["fo:color"]);
        std::string font = p["style:font-name"] ? p["style:font-name"]->getStr().cstr() : "Arial";
        m_span.font = font;
        m_span.bold = is(p, "fo:font-weight", "bold");
        m_span.italic = is(p, "fo:font-style", "italic");
        std::lock_guard<std::mutex> g(g_lock);
        m_span.face = find_font(font, m_span.bold, m_span.italic);
    }
    void closeSpan() override {}
    void openLink(const RVNGPropertyList &) override {}
    void closeLink() override {}

private:
    std::shared_ptr<Style> m_style = std::make_shared<Style>();
    // decoded images by their data: libcdr passes the same buffer for each use of a bitmap (a
    // label sheet repeats one 2000 x 2000 logo 72 times: decoded once, not 72 x 16 MB); the
    // kept RVNGBinaryData keeps the buffer, so its address is not reused by another image
    struct CachedImage {
        librevenge::RVNGBinaryData data;
        std::shared_ptr<Image> img;
    };
    std::map<std::pair<const unsigned char *, unsigned long>, CachedImage> m_images;
    std::shared_ptr<Image> cachedImage(const RVNGProperty *p) {
        const librevenge::RVNGBinaryData *d = p ? p->getBinaryData() : nullptr;
        if (!d || !d->size()) return nullptr;
        auto key = std::make_pair(d->getDataBuffer(), d->size());
        auto f = m_images.find(key);
        if (f != m_images.end()) return f->second.img;
        std::shared_ptr<Image> img = image(p);
        m_images[key] = CachedImage{*d, img};
        return img;
    }
    std::unique_ptr<Text> m_text;
    Span m_span;
    int m_align = 0;
    bool m_inPage = false, m_inMaster = false;

    static float currentX(const plutovg_path_t *path) {
        plutovg_point_t pt = {0, 0};
        plutovg_path_get_current_point(path, &pt.x, &pt.y);
        return pt.x;
    }
    static float currentY(const plutovg_path_t *path) {
        plutovg_point_t pt = {0, 0};
        plutovg_path_get_current_point(path, &pt.x, &pt.y);
        return pt.y;
    }

    void drawPoints(const RVNGPropertyList &p, bool close) {
        const RVNGPropertyListVector *v = p.child("svg:points");
        if (!v || !v->count()) return;
        plutovg_path_t *path = plutovg_path_create();
        for (unsigned long i = 0; i < v->count(); i++) {
            float x = (float)points((*v)[i]["svg:x"]), y = (float)points((*v)[i]["svg:y"]);
            if (i) plutovg_path_line_to(path, x, y);
            else plutovg_path_move_to(path, x, y);
        }
        if (close) plutovg_path_close(path);
        addPath(path);
    }

    void addPath(plutovg_path_t *path) {
        if (!m_inPage || m_inMaster || (m_style->fill == FILL_NONE && !m_style->stroke)) {
            plutovg_path_destroy(path);
            return;
        }
        Item it;
        it.kind = Item::PATH;
        it.style = m_style;
        it.path.reset(path);
        plutovg_rect_t r;
        plutovg_path_extents(path, &r, false);
        float m = m_style->stroke ? std::max(m_style->strokeWidth, 0.5f) * 2 : 0;   // (miters)
        it.bx0 = r.x - m;
        it.by0 = r.y - m;
        it.bx1 = r.x + r.w + m;
        it.by1 = r.y + r.h + m;
        add(std::move(it));
    }

    void add(Item &&it) {
        if (pages.empty()) {   // drawing before any page: give it one
            pages.emplace_back();
            pages.back().w = 595;
            pages.back().h = 842;
        }
        pages.back().items.push_back(std::move(it));
    }

    static void rotatedBox(Item &it, float cx, float cy, float x0, float y0, float x1, float y1, float deg) {
        float c = cosf(deg * (float)M_PI / 180), s = sinf(deg * (float)M_PI / 180);
        float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
        it.bx0 = it.by0 = 1e30f;
        it.bx1 = it.by1 = -1e30f;
        for (int k = 0; k < 4; k++) {
            float dx = xs[k] - cx, dy = ys[k] - cy;
            float x = cx + dx * c - dy * s, y = cy + dx * s + dy * c;
            it.bx0 = std::min(it.bx0, x);
            it.by0 = std::min(it.by0, y);
            it.bx1 = std::max(it.bx1, x);
            it.by1 = std::max(it.by1, y);
        }
    }

    void newLine() {
        if (!m_text) return;
        m_text->lines.emplace_back();
        m_text->lines.back().align = m_align;
        m_text->lines.back().emptyFace = m_span.face;
        m_text->lines.back().emptySize = m_span.size;
    }
    // the text in spans of one face each: characters the span's font lacks come from a fallback font
    void appendText(const std::string &s) {
        if (m_text->lines.empty()) newLine();
        std::lock_guard<std::mutex> g(g_lock);
        size_t start = 0;
        plutovg_font_face_t *runFace = m_span.face;
        for (size_t i = 0; i < s.size();) {
            size_t at = i;
            plutovg_codepoint_t cp = utf8_next(s, i);
            plutovg_font_face_t *f = m_span.face;
            if (f && cp > 0x20 && !plutovg_font_face_has_glyph(f, cp)) {
                plutovg_font_face_t *fb = fallback_font(m_span.font, m_span.bold, m_span.italic, cp);
                if (fb) f = fb;
            }
            if (f != runFace) {
                if (at > start) pushSpan(s.substr(start, at - start), runFace);
                start = at;
                runFace = f;
            }
        }
        if (s.size() > start) pushSpan(s.substr(start), runFace);
    }
    void pushSpan(const std::string &text, plutovg_font_face_t *face) {
        Span sp = m_span;
        sp.text = text;
        sp.face = face;
        m_text->lines.back().spans.push_back(std::move(sp));
    }

    // Line widths, positions and the bounding box. Artistic text (no frame): the first
    // baseline at (x, y), each line aligned on x. Frame text: lines from the frame's top,
    // aligned in its width. Both turn by 'rotate' about (x, y) / the frame's centre.
    void layout(Item &it) {
        Text &t = *it.text;
        std::lock_guard<std::mutex> g(g_lock);
        float maxw = 0, total = 0;
        for (Line &l : t.lines) {
            l.width = 0;
            l.size = 0;
            l.ascent = 0;
            float pen = 0;
            for (Span &s : l.spans) {
                s.width = s.face ? plutovg_font_face_text_extents(s.face, s.size, s.text.data(), (int)s.text.size(),
                                                                  PLUTOVG_TEXT_ENCODING_UTF8, nullptr) * s.scaleX
                                 : 0;
                l.width = std::max(l.width, pen + s.shiftX * s.size + s.width);   // (shifted words reach further)
                pen += s.width;
                l.size = std::max(l.size, s.size);
                if (s.face) {
                    float asc = 0, desc = 0, gap = 0;
                    plutovg_font_face_get_metrics(s.face, s.size, &asc, &desc, &gap, nullptr);
                    l.ascent = std::max(l.ascent, asc);
                    l.height = std::max(l.height, asc - desc);
                }
            }
            if (l.spans.empty() && l.emptySize > 0) {   // an empty line: as high as the line break's font
                l.size = l.emptySize;
                if (l.emptyFace) {
                    float asc = 0, desc = 0, gap = 0;
                    plutovg_font_face_get_metrics(l.emptyFace, l.size, &asc, &desc, &gap, nullptr);
                    l.ascent = asc;
                    l.height = asc - desc;
                }
            }
            if (l.size == 0) l.size = 12;
            if (l.ascent == 0) l.ascent = l.size * 0.8f;
            if (l.height == 0) l.height = l.size * 1.15f;
            maxw = std::max(maxw, l.width);
            total += l.height;
        }
        bool artistic = t.w == 0 && t.h == 0;
        float top = artistic ? t.y - t.lines[0].ascent : t.y;
        float left = artistic ? t.x - maxw : t.x, right = artistic ? t.x + maxw : t.x + std::max(t.w, maxw);
        float cx = artistic ? t.x : t.x + t.w / 2, cy = artistic ? t.y : t.y + t.h / 2;
        rotatedBox(it, cx, cy, left, top, right, top + total + t.lines[0].size, t.rotate);
    }

    std::shared_ptr<Style> parseStyle(const RVNGPropertyList &p) {
        auto st = std::make_shared<Style>();
        st->opacity = (float)number(p["draw:opacity"], 1.0);
        st->evenodd = is(p, "svg:fill-rule", "evenodd");
        if (is(p, "draw:fill", "solid")) {
            st->fill = FILL_SOLID;
            st->fillColor = color(p["draw:fill-color"]);
        } else if (is(p, "draw:fill", "gradient")) {
            bool radial = is(p, "draw:style", "radial") || is(p, "draw:style", "square") || is(p, "draw:style", "rectangular") ||
                          is(p, "draw:style", "ellipsoid");
            bool axial = is(p, "draw:style", "axial");
            st->fill = radial ? FILL_RADIAL : FILL_LINEAR;
            st->angle = (float)number(p["draw:angle"]);
            Color c0 = color(p["draw:start-color"]), c1 = color(p["draw:end-color"], Color{1, 1, 1, 1});
            const RVNGPropertyListVector *v = p.child("svg:linearGradient");
            if (!v) v = p.child("svg:radialGradient");
            auto stop = [](float off, Color c, float a) {
                plutovg_gradient_stop_t s;
                s.offset = off;
                s.color.r = c.r;
                s.color.g = c.g;
                s.color.b = c.b;
                s.color.a = a;
                return s;
            };
            if (v && v->count()) {
                for (unsigned long i = 0; i < v->count(); i++)
                    st->stops.push_back(stop((float)number((*v)[i]["svg:offset"]), color((*v)[i]["svg:stop-color"]),
                                             (float)number((*v)[i]["svg:stop-opacity"], 1.0)));
            } else if (radial) {   // (as librevenge's SVG generator: the end colour in the centre)
                st->stops = {stop(0, c1, 1), stop(1, c0, 1)};
            } else if (axial) {
                st->stops = {stop(0, c1, 1), stop(.5f, c0, 1), stop(1, c1, 1)};
            } else {
                st->stops = {stop(0, c0, 1), stop(1, c1, 1)};
            }
            if (radial) {
                st->cx = (float)number(p["svg:cx"], .5);
                st->cy = (float)number(p["svg:cy"], .5);
                st->r = p["draw:border"] ? (float)(1 - number(p["draw:border"])) : 1.f;
                if (st->r <= 0) st->r = .01f;
            }
        } else if (is(p, "draw:fill", "bitmap")) {
            st->tile = cachedImage(p["draw:fill-image"]);
            if (st->tile) {
                st->fill = FILL_BITMAP;
                st->tileRel = p["svg:width"] && p["svg:width"]->getUnit() == librevenge::RVNG_PERCENT;
                st->tileW = st->tileRel ? (float)number(p["svg:width"]) : (float)points(p["svg:width"]);
                st->tileH = st->tileRel ? (float)number(p["svg:height"]) : (float)points(p["svg:height"]);
                st->refX = (float)number(p["draw:fill-image-ref-point-x"]);
                st->refY = (float)number(p["draw:fill-image-ref-point-y"]);
            }
        }
        if (p["draw:stroke"] && !is(p, "draw:stroke", "none")) {
            st->stroke = true;
            st->strokeColor = color(p["svg:stroke-color"]);
            st->strokeColor.a = (float)number(p["svg:stroke-opacity"], 1.0);
            st->strokeWidth = (float)points(p["svg:stroke-width"]);
            if (is(p, "svg:stroke-linecap", "round")) st->cap = PLUTOVG_LINE_CAP_ROUND;
            else if (is(p, "svg:stroke-linecap", "square")) st->cap = PLUTOVG_LINE_CAP_SQUARE;
            if (is(p, "svg:stroke-linejoin", "round")) st->join = PLUTOVG_LINE_JOIN_ROUND;
            else if (is(p, "svg:stroke-linejoin", "bevel")) st->join = PLUTOVG_LINE_JOIN_BEVEL;
            if (is(p, "draw:stroke", "dash")) {   // ODF: dots1 x dots1-length, dots2 x dots2-length, all + distance
                int n1 = p["draw:dots1"] ? p["draw:dots1"]->getInt() : 0, n2 = p["draw:dots2"] ? p["draw:dots2"]->getInt() : 0;
                float l1 = (float)points(p["draw:dots1-length"]), l2 = (float)points(p["draw:dots2-length"]);
                float gap = (float)points(p["draw:distance"]);
                for (int k = 0; k < n1 && k < 64; k++) st->dashes.insert(st->dashes.end(), {l1, gap});
                for (int k = 0; k < n2 && k < 64; k++) st->dashes.insert(st->dashes.end(), {l2, gap});
                bool visible = false;
                for (float d : st->dashes) visible |= d > 0;
                if (!visible) st->dashes.clear();
            }
        }
        return st;
    }
};

// --- drawing -----------------------------------------------------------------

void setColor(plutovg_canvas_t *c, Color col, float alpha) { plutovg_canvas_set_rgba(c, col.r, col.g, col.b, col.a * alpha); }

// Sets the fill paint for an item with bounding box (x, y, w, h).
bool setFill(plutovg_canvas_t *c, const Style &st, float x, float y, float w, float h) {
    switch (st.fill) {
    case FILL_SOLID: setColor(c, st.fillColor, st.opacity); return true;
    case FILL_LINEAR:
    case FILL_RADIAL: {
        if (st.stops.empty() || w <= 0 || h <= 0) return false;
        plutovg_matrix_t m;   // bounding box units (librevenge's SVG generator: objectBoundingBox)
        plutovg_matrix_init_translate(&m, x, y);
        plutovg_matrix_scale(&m, w, h);
        if (st.fill == FILL_LINEAR) {   // top to bottom, turned by -angle about the centre
            plutovg_matrix_translate(&m, .5f, .5f);
            plutovg_matrix_rotate(&m, (float)(-st.angle * M_PI / 180));
            plutovg_matrix_translate(&m, -.5f, -.5f);
            plutovg_canvas_set_linear_gradient(c, 0, 0, 0, 1, PLUTOVG_SPREAD_METHOD_PAD, st.stops.data(), (int)st.stops.size(), &m);
        } else {
            plutovg_canvas_set_radial_gradient(c, st.cx, st.cy, st.r, st.cx, st.cy, 0, PLUTOVG_SPREAD_METHOD_PAD, st.stops.data(),
                                               (int)st.stops.size(), &m);
        }
        plutovg_canvas_set_opacity(c, st.opacity);
        return true;
    }
    case FILL_BITMAP: {
        int iw = plutovg_surface_get_width(st.tile->s), ih = plutovg_surface_get_height(st.tile->s);
        float tw = st.tileRel ? st.tileW * w : st.tileW, th = st.tileRel ? st.tileH * h : st.tileH;
        if (iw <= 0 || ih <= 0 || tw <= 0 || th <= 0) return false;
        plutovg_matrix_t m;   // tiles from the bottom left corner, shifted by the reference point
        plutovg_matrix_init_translate(&m, x + st.refX * tw, y + h - th + st.refY * th);
        plutovg_matrix_scale(&m, tw / iw, th / ih);
        plutovg_canvas_set_texture(c, st.tile->s, PLUTOVG_TEXTURE_TYPE_TILED, st.opacity, &m);
        return true;
    }
    default: return false;
    }
}

void drawItem(plutovg_canvas_t *c, const Item &it, float scale) {
    plutovg_canvas_save(c);
    if (it.kind == Item::PATH) {
        const Style &st = *it.style;
        plutovg_rect_t r;
        plutovg_path_extents(it.path.get(), &r, false);
        if (st.fill != FILL_NONE && setFill(c, st, r.x, r.y, r.w, r.h)) {
            plutovg_canvas_set_fill_rule(c, st.evenodd ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
            plutovg_canvas_fill_path(c, it.path.get());
        }
        if (st.stroke) {
            plutovg_canvas_set_opacity(c, 1);
            setColor(c, st.strokeColor, 1);
            // never thinner than one device pixel (as CorelDRAW and PDF viewers draw them):
            // hairlines (width 0) and the many 0.1 pt lines of architectural plans would
            // otherwise fade to a faint grey when zoomed out
            plutovg_canvas_set_line_width(c, std::max(st.strokeWidth, 1 / scale));
            plutovg_canvas_set_line_cap(c, st.cap);
            plutovg_canvas_set_line_join(c, st.join);
            if (!st.dashes.empty()) plutovg_canvas_set_dash(c, 0, st.dashes.data(), (int)st.dashes.size());
            plutovg_canvas_stroke_path(c, it.path.get());
        }
    } else if (it.kind == Item::IMAGE) {
        if (it.path) plutovg_canvas_clip_path(c, it.path.get());   // (before the image's own transform)
        int iw = plutovg_surface_get_width(it.img->s), ih = plutovg_surface_get_height(it.img->s);
        float cx = it.x + it.w / 2, cy = it.y + it.h / 2;
        plutovg_canvas_translate(c, cx, cy);
        plutovg_canvas_scale(c, it.mirrorX ? -1 : 1, it.mirrorY ? -1 : 1);
        plutovg_canvas_rotate(c, (float)(it.rotate * M_PI / 180));
        plutovg_canvas_translate(c, -cx, -cy);
        plutovg_matrix_t m;
        plutovg_matrix_init_translate(&m, it.x, it.y);
        plutovg_matrix_scale(&m, it.w / iw, it.h / ih);
        plutovg_canvas_set_texture(c, it.img->s, PLUTOVG_TEXTURE_TYPE_PLAIN, 1, &m);
        plutovg_canvas_fill_rect(c, it.x, it.y, it.w, it.h);
    } else {
        const Text &t = *it.text;
        bool artistic = t.w == 0 && t.h == 0;
        float cx = artistic ? t.x : t.x + t.w / 2, cy = artistic ? t.y : t.y + t.h / 2;
        if (t.rotate != 0) {
            plutovg_canvas_translate(c, cx, cy);
            plutovg_canvas_rotate(c, (float)(t.rotate * M_PI / 180));
            plutovg_canvas_translate(c, -cx, -cy);
        }
        float base = artistic ? t.y : t.y + t.lines[0].ascent;
        for (size_t i = 0; i < t.lines.size(); i++) {
            const Line &l = t.lines[i];
            if (i) base += l.height;
            float x;
            if (artistic) x = l.align == 1 ? t.x - l.width / 2 : l.align == 2 ? t.x - l.width : t.x;
            else x = l.align == 1 ? t.x + (t.w - l.width) / 2 : l.align == 2 ? t.x + t.w - l.width : t.x;
            for (const Span &s : l.spans) {
                if (s.face) {
                    plutovg_canvas_set_font(c, s.face, s.size);
                    setColor(c, s.color, 1);
                    float sx = x + s.shiftX * s.size, sy = base - s.shiftY * s.size;   // (the next span starts unshifted)
                    if (s.scaleX != 1) {
                        plutovg_canvas_save(c);
                        plutovg_canvas_translate(c, sx, sy);
                        plutovg_canvas_scale(c, s.scaleX, 1);
                        plutovg_canvas_fill_text(c, s.text.data(), (int)s.text.size(), PLUTOVG_TEXT_ENCODING_UTF8, 0, 0);
                        plutovg_canvas_restore(c);
                    } else
                        plutovg_canvas_fill_text(c, s.text.data(), (int)s.text.size(), PLUTOVG_TEXT_ENCODING_UTF8, sx, sy);
                }
                x += s.width;
            }
        }
    }
    plutovg_canvas_restore(c);
}

} // namespace

struct nitrocdr {
    std::vector<Page> pages;
};

nitrocdr *nitrocdr_load(const void *data, size_t len) {
    librevenge::RVNGStringStream in(static_cast<const unsigned char *>(data), (unsigned)len);
    Painter painter;
    bool ok = false;
    if (libcdr::CDRDocument::isSupported(&in)) {
        in.seek(0, librevenge::RVNG_SEEK_SET);
        ok = libcdr::CDRDocument::parse(&in, &painter);
    } else {
        in.seek(0, librevenge::RVNG_SEEK_SET);
        if (libcdr::CMXDocument::isSupported(&in)) {
            in.seek(0, librevenge::RVNG_SEEK_SET);
            ok = libcdr::CMXDocument::parse(&in, &painter);
        }
    }
    if (!ok || painter.pages.empty()) return nullptr;
    // the shown area is the drawing's bounding box (+ a small margin), not the page set in the
    // file: a small logo on an A4 page fills the view, a scan much larger than its page shows
    // in full (an empty page keeps its size). A drawing of several pages gets one area, the
    // bounding box of all its pages: what lies above each other on them (the floors of a
    // building) stays in place when paging.
    double ux0 = 1e30, uy0 = 1e30, ux1 = -1e30, uy1 = -1e30;
    std::vector<std::array<double, 4>> boxes;
    for (const Page &pg : painter.pages) {
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (const Item &it : pg.items) {
            x0 = std::min(x0, (double)it.bx0);
            y0 = std::min(y0, (double)it.by0);
            x1 = std::max(x1, (double)it.bx1);
            y1 = std::max(y1, (double)it.by1);
        }
        boxes.push_back({x0, y0, x1, y1});
        if (x1 <= x0 || y1 <= y0) continue;
        ux0 = std::min(ux0, x0);
        uy0 = std::min(uy0, y0);
        ux1 = std::max(ux1, x1);
        uy1 = std::max(uy1, y1);
    }
    for (size_t i = 0; i < painter.pages.size(); i++) {
        Page &pg = painter.pages[i];
        double x0 = boxes[i][0], y0 = boxes[i][1], x1 = boxes[i][2], y1 = boxes[i][3];
        if (painter.pages.size() > 1 && ux1 > ux0 && uy1 > uy0) {
            x0 = ux0;
            y0 = uy0;
            x1 = ux1;
            y1 = uy1;
        }
        if (x1 <= x0 || y1 <= y0) continue;
        double m = std::max(2.0, 0.015 * std::max(x1 - x0, y1 - y0));
        pg.x = x0 - m;
        pg.y = y0 - m;
        pg.w = x1 - x0 + 2 * m;
        pg.h = y1 - y0 + 2 * m;
    }
    return new nitrocdr{std::move(painter.pages)};
}

void nitrocdr_free(nitrocdr *cdr) {
    std::lock_guard<std::mutex> g(g_lock);   // (surfaces / paths only; faces stay in the cache)
    delete cdr;
}

int nitrocdr_pages(const nitrocdr *cdr) { return (int)cdr->pages.size(); }

void nitrocdr_size(const nitrocdr *cdr, int page, double *w, double *h) {
    *w = cdr->pages[page].w;
    *h = cdr->pages[page].h;
}

int nitrocdr_render(nitrocdr *cdr, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0) {
    if (page < 0 || page >= (int)cdr->pages.size() || w <= 0 || h <= 0 || pitch < (size_t)w * 4 || !(scale > 0)) return -1;
    for (int y = 0; y < h; y++) memset(bgra + (size_t)y * pitch, 0xFF, (size_t)w * 4);   // white, opaque
    std::lock_guard<std::mutex> g(g_lock);
    plutovg_surface_t *surface = plutovg_surface_create_for_data(bgra, w, h, (int)pitch);
    plutovg_canvas_t *c = plutovg_canvas_create(surface);
    plutovg_canvas_scale(c, (float)scale, (float)scale);
    x0 += cdr->pages[page].x;   // (the shown area's origin)
    y0 += cdr->pages[page].y;
    plutovg_canvas_translate(c, (float)-x0, (float)-y0);
    float vx0 = (float)x0, vy0 = (float)y0, vx1 = (float)(x0 + w / scale), vy1 = (float)(y0 + h / scale);
    for (const Item &it : cdr->pages[page].items)
        if (it.bx1 >= vx0 && it.bx0 <= vx1 && it.by1 >= vy0 && it.by0 <= vy1) drawItem(c, it, (float)scale);
    plutovg_canvas_destroy(c);
    plutovg_surface_destroy(surface);
    return 0;
}
