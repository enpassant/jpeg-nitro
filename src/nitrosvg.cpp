// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrosvg: C interface to lunasvg. See nitrosvg.h.
#include "nitrosvg.h"

#include <lunasvg.h>

#include <cctype>
#include <cstring>
#include <string>
#include <memory>
#include <mutex>

struct nitrosvg {
    std::unique_ptr<lunasvg::Document> doc;
    double w, h;     // intrinsic size
    double ox, oy;   // its top-left corner in document units (bounding box case)
};

static std::mutex g_lock;   // every lunasvg call: the shared font cache has no lock of its own here

// Fixes applied to the text before lunasvg parses it (only when the file needs them):
//
// "<svg:svg xmlns:svg=...>" (LibreOffice): valid XML with a namespace prefix, but lunasvg only
// knows plain element names: the "svg:" prefix of the elements is dropped.
static std::string unprefix(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        out += s[i];
        if (s[i] == '<') {
            if (i + 1 < s.size() && s[i + 1] == '/') out += s[++i];
            if (!s.compare(i + 1, 4, "svg:")) i += 4;
        }
    }
    return out;
}

// CSS custom properties, "var(--vscode-editor-background, #1E1E1E)" (VS Code's images): nothing
// defines the property in a standalone file, so a browser uses the fallback, or without one the
// inherited value (these files have fill="none" on the root: invisible); lunasvg doesn't know
// var() at all (black squares). Replaced by the fallback, or by "inherit".
static std::string var_fallbacks(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    for (size_t v; (v = s.find("var(--", i)) != std::string::npos;) {
        out.append(s, i, v - i);
        size_t j = v + 4, comma = std::string::npos;
        int depth = 1;
        for (; j < s.size() && depth; j++) {
            if (s[j] == '(') depth++;
            else if (s[j] == ')') depth--;
            else if (s[j] == ',' && depth == 1 && comma == std::string::npos) comma = j;
            else if (s[j] == '"' || s[j] == '<' || s[j] == ';') break;   // not a well-formed var()
        }
        if (depth) {   // damaged: leave it
            out.append(s, v, 4);
            i = v + 4;
            continue;
        }
        if (comma == std::string::npos) {   // no fallback: the inherited value
            out += "inherit";
            i = j;
            continue;
        }
        size_t a = comma + 1, b = j - 1;   // fallback: (comma, closing parenthesis)
        while (a < b && isspace((unsigned char)s[a])) a++;
        while (b > a && isspace((unsigned char)s[b - 1])) b--;
        out += var_fallbacks(s.substr(a, b - a));   // the fallback may be a var() itself
        i = j;
    }
    out.append(s, i, std::string::npos);
    return out;
}

// Line breaks in text: SVG 1.1 (xml:space="default") removes them, newer renderers (lunasvg,
// resvg, CSS white-space) turn them into spaces. Old exporters (LibreOffice) split words into
// tspans on separate lines, "N\n...\népszínház" -> "N épszínház": removed, as SVG 1.1 says.
// Not when the file uses xml:space="preserve" (or 'preserve') anywhere (there they do become spaces).
static std::string text_newlines(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    int depth = 0;   // inside <text> elements
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '<') {
            size_t e = s.find('>', i);
            if (e == std::string::npos) e = s.size() - 1;
            bool close = i + 1 < s.size() && s[i + 1] == '/';
            size_t n = i + 1 + close;
            if (!s.compare(n, 4, "svg:")) n += 4;
            if (!s.compare(n, 4, "text") && n + 4 < s.size() &&
                (isspace((unsigned char)s[n + 4]) || s[n + 4] == '>' || s[n + 4] == '/')) {
                if (close) depth = depth > 0 ? depth - 1 : 0;
                else if (s[e - 1] != '/') depth++;
            }
            out.append(s, i, e - i + 1);
            i = e;
            continue;
        }
        if (depth > 0 && (c == '\n' || c == '\r')) continue;
        out += c;
    }
    return out;
}

nitrosvg *nitrosvg_load(const void *data, size_t len) {
    std::string src(static_cast<const char *>(data), len);
    if (src.find("<svg:svg") != std::string::npos) src = unprefix(src);
    if (src.find("var(--") != std::string::npos) src = var_fallbacks(src);
    if (src.find("<text") != std::string::npos && src.find("xml:space=\"preserve\"") == std::string::npos &&
        src.find("xml:space='preserve'") == std::string::npos)
        src = text_newlines(src);
    const char *s = src.data();
    len = src.size();
    std::lock_guard<std::mutex> g(g_lock);
    auto doc = lunasvg::Document::loadFromData(s, len);
    if (!doc) return nullptr;
    double w = doc->width(), h = doc->height(), ox = 0, oy = 0;
    if (!(w > 0 && h > 0)) {   // no width / height / viewBox: what it draws decides
        lunasvg::Box b = doc->boundingBox();
        if (!(b.w > 0 && b.h > 0)) return nullptr;
        w = b.w;
        h = b.h;
        ox = b.x;
        oy = b.y;
    }
    return new nitrosvg{std::move(doc), w, h, ox, oy};
}

void nitrosvg_free(nitrosvg *svg) {
    std::lock_guard<std::mutex> g(g_lock);
    delete svg;
}

void nitrosvg_size(const nitrosvg *svg, double *w, double *h) {
    *w = svg->w;
    *h = svg->h;
}

int nitrosvg_render(nitrosvg *svg, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0) {
    if (w <= 0 || h <= 0 || pitch < (size_t)w * 4 || !(scale > 0)) return -1;
    for (int y = 0; y < h; y++) memset(bgra + (size_t)y * pitch, 0xFF, (size_t)w * 4);   // white, opaque
    std::lock_guard<std::mutex> g(g_lock);
    lunasvg::Bitmap bitmap(bgra, w, h, (int)pitch);   // ARGB32 premultiplied = BGRA bytes (little endian)
    if (bitmap.isNull()) return -1;
    lunasvg::Matrix m((float)scale, 0, 0, (float)scale, (float)(-(x0 + svg->ox) * scale), (float)(-(y0 + svg->oy) * scale));
    svg->doc->render(bitmap, m);
    return 0;
}
