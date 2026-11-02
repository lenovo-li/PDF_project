#include "Analyze.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace pdf {

namespace {
bool SameStyle(const TextRun& r, const TextChar& c) {
    return r.fontName == c.fontName && std::fabs(r.fontSize - c.fontSize) < 0.5f &&
           r.weight == c.weight && r.italic == c.italic && r.r == c.r && r.g == c.g && r.b == c.b;
}

TextRun RunFor(const TextChar& c) {
    TextRun r;
    r.fontName = c.fontName;
    r.fontSize = c.fontSize;
    r.weight = c.weight;
    r.italic = c.italic;
    r.r = c.r;
    r.g = c.g;
    r.b = c.b;
    return r;
}

// Characters of one line, already in left-to-right order, into runs.
void AppendToLine(TextLine& line, const TextChar& c) {
    // A space takes the style of the run it continues, so "bold word" does
    // not split into three runs around the space.
    if (c.unicode == U' ' && !line.runs.empty()) {
        line.runs.back().text += U' ';
        return;
    }
    if (line.runs.empty() || !SameStyle(line.runs.back(), c)) line.runs.push_back(RunFor(c));
    line.runs.back().text += c.unicode;
}

float LineFontSize(const TextLine& l) {
    float s = 0;
    for (const TextRun& r : l.runs) s = std::max(s, r.fontSize);
    return s > 0 ? s : 12.0f;
}

bool EndsWithSpace(const TextLine& l) {
    return !l.runs.empty() && !l.runs.back().text.empty() && l.runs.back().text.back() == U' ';
}

void TrimTrailingSpace(TextLine& l) {
    while (!l.runs.empty()) {
        std::u32string& t = l.runs.back().text;
        while (!t.empty() && t.back() == U' ') t.pop_back();
        if (!t.empty()) break;
        l.runs.pop_back();
    }
}

// std::clamp with lo > hi is undefined; a page narrower than 3 × 18 pt has
// exactly that. The upper bound wins then.
float SafeClamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

// Inserts each picture before the first paragraph whose first line sits
// below the picture's top edge; reading order is top to bottom.
void PlaceImages(std::vector<PageImage*>& pics, std::vector<Paragraph>* paras) {
    std::stable_sort(pics.begin(), pics.end(),
                     [](const PageImage* a, const PageImage* b) { return a->top < b->top; });
    std::size_t at = 0;
    for (PageImage* img : pics) {
        // A paragraph's top is its first baseline minus about one font size.
        while (at < paras->size()) {
            const Paragraph& p = (*paras)[at];
            if (p.lines.empty()) { ++at; continue; }  // a picture placed already
            const TextLine& first = p.lines.front();
            float size = 0;
            for (const TextRun& r : first.runs) size = std::max(size, r.fontSize);
            if (first.y - size > img->top) break;
            ++at;
        }
        Paragraph pic;
        pic.imagePng = std::move(img->png);
        pic.imageWidthPt = img->right - img->left;
        pic.imageHeightPt = img->bottom - img->top;
        paras->insert(paras->begin() + static_cast<std::ptrdiff_t>(at), std::move(pic));
        ++at;
    }
}

float Median(std::vector<float> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}
}  // namespace

std::vector<TextLine> BuildLines(std::vector<TextChar> chars) {
    std::vector<TextLine> lines;
    if (chars.empty()) return lines;
    // Band by baseline: sort top to bottom, then a character joins the band
    // while its baseline is within half the band's font size of the band's.
    std::stable_sort(chars.begin(), chars.end(),
                     [](const TextChar& a, const TextChar& b) { return a.baseline < b.baseline; });
    std::vector<std::vector<TextChar>> bands;
    float bandBase = 0, bandSize = 0;
    for (TextChar& c : chars) {
        const float size = c.fontSize > 0 ? c.fontSize : 12.0f;
        if (bands.empty() || std::fabs(c.baseline - bandBase) > 0.5f * std::max(bandSize, size)) {
            bands.emplace_back();
            bandBase = c.baseline;
            bandSize = size;
        } else {
            bandSize = std::max(bandSize, size);
        }
        bands.back().push_back(std::move(c));
    }
    for (auto& band : bands) {
        // Stable: a generated space keeps its place right after its glyph,
        // which has the same `left` as the space's.
        std::stable_sort(band.begin(), band.end(),
                         [](const TextChar& a, const TextChar& b) { return a.left < b.left; });
        TextLine line;
        line.left = band.front().left;
        line.right = band.front().right;
        float baseSum = 0;
        int baseCount = 0;
        const TextChar* prev = nullptr;
        for (const TextChar& c : band) {
            if (c.unicode == U' ' && (line.runs.empty() || EndsWithSpace(line))) continue;
            if (prev && c.unicode != U' ' && prev->unicode != U' ') {
                // No space in the text layer but a visible gap: insert one.
                const float gap = c.left - prev->right;
                if (gap > 0.25f * std::max(c.fontSize, prev->fontSize)) {
                    TextChar sp = *prev;
                    sp.unicode = U' ';
                    AppendToLine(line, sp);
                }
            }
            AppendToLine(line, c);
            if (c.unicode != U' ') {
                line.right = std::max(line.right, c.right);
                line.left = std::min(line.left, c.left);
                baseSum += c.baseline;
                ++baseCount;
            }
            prev = &c;
        }
        TrimTrailingSpace(line);
        if (line.runs.empty()) continue;
        line.y = baseCount ? baseSum / baseCount : band.front().baseline;
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<Paragraph> BuildParagraphs(const std::vector<TextLine>& lines, float columnLeft,
                                       float columnRight) {
    std::vector<Paragraph> paras;
    if (lines.empty()) return paras;
    // Typical line pitch: the median distance between consecutive baselines
    // that are close enough to be in the same paragraph at all.
    std::vector<float> pitches;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const float d = lines[i].y - lines[i - 1].y;
        if (d > 0 && d < 3.0f * LineFontSize(lines[i])) pitches.push_back(d);
    }
    const float pitch = pitches.empty() ? 1.2f * LineFontSize(lines.front()) : Median(pitches);
    const float width = std::max(1.0f, columnRight - columnLeft);

    auto startsNew = [&](const TextLine& prev, const TextLine& cur) {
        const float gap = cur.y - prev.y;
        if (gap > 1.6f * pitch || gap < 0) return true;
        const float ps = LineFontSize(prev), cs = LineFontSize(cur);
        if (std::fabs(ps - cs) > 0.2f * std::max(ps, cs)) return true;
        // Previous line stops well before the right edge: end of paragraph.
        if (columnRight - prev.right > 0.15f * width) return true;
        // This line starts indented relative to the previous one.
        if (cur.left - prev.left > 1.5f * cs) return true;
        return false;
    };

    float prevBottom = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i == 0 || startsNew(lines[i - 1], lines[i])) {
            Paragraph p;
            if (prevBottom >= 0) {
                // Extra space beyond one normal line pitch.
                p.spacingBefore = std::max(0.0f, lines[i].y - prevBottom - pitch);
            }
            paras.push_back(std::move(p));
        }
        paras.back().lines.push_back(lines[i]);
        prevBottom = lines[i].y;
    }

    for (Paragraph& p : paras) {
        const TextLine& first = p.lines.front();
        const float size = LineFontSize(first);
        float minLeft = first.left, maxRight = first.right;
        for (const TextLine& l : p.lines) {
            minLeft = std::min(minLeft, l.left);
            maxRight = std::max(maxRight, l.right);
        }
        // Body lines set the left edge; the first line's offset from them is
        // the first-line indent (positive) or a hanging indent (negative).
        const float bodyLeft = p.lines.size() > 1 ? p.lines[1].left : first.left;
        p.leftIndent = bodyLeft;
        p.firstLineIndent = first.left - bodyLeft;
        if (std::fabs(p.firstLineIndent) < 2.0f) p.firstLineIndent = 0;

        // Alignment from the margins on both sides of a single-line
        // paragraph, or of all lines of a longer one.
        const float lm = minLeft - columnLeft, rm = columnRight - maxRight;
        if (lm > 0.1f * width && std::fabs(lm - rm) < 0.08f * width) {
            p.align = Alignment::Center;
            p.leftIndent = 0;
            p.firstLineIndent = 0;
        } else if (lm > 0.3f * width && rm < 0.05f * width) {
            p.align = Alignment::Right;
            p.leftIndent = 0;
            p.firstLineIndent = 0;
        }

        // Line spacing as a multiple of single spacing (~1.2 × font size).
        if (p.lines.size() > 1) {
            std::vector<float> d;
            for (std::size_t i = 1; i < p.lines.size(); ++i) d.push_back(p.lines[i].y - p.lines[i - 1].y);
            p.lineSpacing = std::clamp(Median(d) / (1.2f * size), 0.8f, 3.0f);
        }
    }
    return paras;
}

float FindGutter(const std::vector<TextChar>& chars, float pageWidth) {
    if (chars.size() < 40 || pageWidth < 200) return -1;
    // Occupancy in 1 pt bins across the page, through a difference array:
    // O(chars + width) however wide a hostile glyph box is.
    const int bins = static_cast<int>(std::ceil(pageWidth));
    std::vector<int> cover(static_cast<std::size_t>(bins) + 1, 0);
    for (const TextChar& c : chars) {
        if (c.unicode == U' ') continue;
        const int a = std::clamp(static_cast<int>(c.left), 0, bins);
        const int b = std::clamp(static_cast<int>(std::ceil(c.right)), 0, bins);
        if (a >= b) continue;
        ++cover[static_cast<std::size_t>(a)];
        --cover[static_cast<std::size_t>(b)];
    }
    for (int x = 1; x <= bins; ++x) cover[static_cast<std::size_t>(x)] += cover[static_cast<std::size_t>(x) - 1];
    // Widest empty run inside the middle 30–70 %.
    const int lo = static_cast<int>(pageWidth * 0.3f), hi = static_cast<int>(pageWidth * 0.7f);
    int bestStart = -1, bestLen = 0, runStart = -1;
    for (int x = lo; x <= hi; ++x) {
        const bool empty = x < bins && cover[static_cast<std::size_t>(x)] == 0;
        if (empty && runStart < 0) runStart = x;
        if ((!empty || x == hi) && runStart >= 0) {
            const int len = x - runStart;
            if (len > bestLen) {
                bestLen = len;
                bestStart = runStart;
            }
            runStart = -1;
        }
    }
    if (bestLen < kMinGutterPt) return -1;
    const float gutter = bestStart + bestLen / 2.0f;
    // Both sides must hold a real share of the text, or this is just a
    // ragged right margin of a single column.
    std::size_t left = 0, right = 0;
    for (const TextChar& c : chars) {
        if (c.unicode == U' ') continue;
        (c.right <= gutter ? left : right) += 1;
    }
    const std::size_t total = left + right;
    if (left < total / 5 || right < total / 5) return -1;
    return gutter;
}

AnalyzedDocument Analyze(std::vector<ExtractedPage> pages) {
    AnalyzedDocument doc;
    for (ExtractedPage& page : pages) {
        // A new section when the page size changes.
        if (doc.sections.empty() ||
            std::fabs(doc.sections.back().widthPt - page.widthPt) > 1.0f ||
            std::fabs(doc.sections.back().heightPt - page.heightPt) > 1.0f) {
            Section s;
            s.widthPt = page.widthPt;
            s.heightPt = page.heightPt;
            s.marginLeft = s.marginRight = s.marginTop = s.marginBottom = -1;  // set below
            doc.sections.push_back(std::move(s));
        }
        Section& sec = doc.sections.back();
        const bool firstOfSection = sec.paragraphs.empty();
        const std::size_t before = sec.paragraphs.size();

        if (!page.pagePng.empty()) {
            Paragraph p;
            p.imagePng = std::move(page.pagePng);
            p.imageWidthPt = page.widthPt;
            p.imageHeightPt = page.heightPt;
            sec.paragraphs.push_back(std::move(p));
        } else if (!page.chars.empty()) {
            // Text block extent, for margins.
            float minX = page.widthPt, maxX = 0, minY = page.heightPt, maxY = 0;
            for (const TextChar& c : page.chars) {
                if (c.unicode == U' ') continue;
                minX = std::min(minX, c.left);
                maxX = std::max(maxX, c.right);
                minY = std::min(minY, c.baseline - c.fontSize);
                maxY = std::max(maxY, c.baseline);
            }
            auto setMin = [](float& m, float v) { m = m < 0 ? v : std::min(m, v); };
            setMin(sec.marginLeft, SafeClamp(minX, 18.0f, page.widthPt / 3));
            setMin(sec.marginRight, SafeClamp(page.widthPt - maxX, 18.0f, page.widthPt / 3));
            setMin(sec.marginTop, SafeClamp(minY, 18.0f, page.heightPt / 3));
            setMin(sec.marginBottom, SafeClamp(page.heightPt - maxY, 18.0f, page.heightPt / 3));

            const float gutter = FindGutter(page.chars, page.widthPt);
            std::vector<std::vector<TextChar>> columns(gutter > 0 ? 2 : 1);
            for (const TextChar& c : page.chars)
                columns[gutter > 0 && c.left >= gutter ? 1 : 0].push_back(c);
            // Pictures go to the column holding their centre.
            std::vector<std::vector<PageImage*>> pics(columns.size());
            for (PageImage& img : page.images)
                pics[gutter > 0 && (img.left + img.right) / 2 >= gutter ? 1 : 0].push_back(&img);
            for (std::size_t ci = 0; ci < columns.size(); ++ci) {
                auto& col = columns[ci];
                std::vector<Paragraph> paras;
                if (!col.empty()) {
                    float l = page.widthPt, r = 0;
                    for (const TextChar& c : col) {
                        if (c.unicode == U' ') continue;
                        l = std::min(l, c.left);
                        r = std::max(r, c.right);
                    }
                    paras = BuildParagraphs(BuildLines(std::move(col)), l, r);
                }
                PlaceImages(pics[ci], &paras);
                for (Paragraph& p : paras) sec.paragraphs.push_back(std::move(p));
            }
        }
        // Each PDF page starts on a new Word page (the first of a section
        // already does, through the section break).
        if (!firstOfSection && sec.paragraphs.size() > before)
            sec.paragraphs[before].pageBreakBefore = true;
    }
    // Sections of scans only never saw text: small margins so the page image
    // is shown nearly full size.
    for (Section& s : doc.sections) {
        if (s.marginLeft < 0) s.marginLeft = 18;
        if (s.marginRight < 0) s.marginRight = 18;
        if (s.marginTop < 0) s.marginTop = 18;
        if (s.marginBottom < 0) s.marginBottom = 18;
    }
    return doc;
}

}  // namespace pdf
