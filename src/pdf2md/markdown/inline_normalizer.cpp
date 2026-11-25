#include "pdf2md/markdown/inline_normalizer.h"

#include "pdf2md/utf8.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pdf2md::markdown {

namespace {

constexpr std::size_t kMinCompoundPartLen = 4U;
constexpr std::size_t kMaxUrlSchemeLen = 8U;

constexpr bool is_word_letter_cp(char32_t cp) noexcept {
    if (is_ascii_alpha(cp)) {
        return true;
    }
    if (cp >= 0x00C0U && cp <= 0x024FU && cp != 0x00D7U && cp != 0x00F7U) {
        return true;
    }
    if (cp >= 0x0370U && cp <= 0x1FFFU) {
        return true;
    }
    if ((cp >= 0x3040U && cp <= 0x9FFFU) || (cp >= 0xAC00U && cp <= 0xD7AFU) ||
        (cp >= 0xF900U && cp <= 0xFAFFU) || (cp >= 0x20000U && cp <= 0x2FA1FU)) {
        return true;
    }
    return (cp >= 0xFF21U && cp <= 0xFF3AU) || (cp >= 0xFF41U && cp <= 0xFF5AU) ||
           (cp >= 0xFF66U && cp <= 0xFF9FU);
}

constexpr std::array<std::u32string_view, 18> kBoundPrefixes = {
    U"anti",  U"counter", U"hyper", U"hypo",  U"inter", U"intra",
    U"macro", U"micro",   U"mono",  U"multi", U"poly",  U"proto",
    U"pseudo", U"quasi",  U"semi",  U"super", U"trans", U"ultra",
};
static_assert(std::is_sorted(kBoundPrefixes.begin(), kBoundPrefixes.end()));

std::u32string lower_ascii(std::u32string_view s) {
    std::u32string out(s);
    for (char32_t& c : out) {
        if (is_ascii_upper(c)) {
            c = c - U'A' + U'a';
        }
    }
    return out;
}

}  // namespace

WordSet build_document_vocab(const Document& doc) {
    WordSet vocab;
    std::vector<std::u32string> toks;
    std::u32string cur;
    for (const Block& b : doc.blocks) {
        for (const Line& ln : b.lines) {
            toks.clear();
            cur.clear();
            for (const char32_t cp : ln.plain) {
                if (is_ascii_alpha(cp)) {
                    cur += is_ascii_upper(cp) ? static_cast<char32_t>(cp - U'A' + U'a') : cp;
                    continue;
                }
                if (!cur.empty()) {
                    toks.push_back(std::move(cur));
                    cur.clear();
                }
            }
            if (!cur.empty()) {
                toks.push_back(std::move(cur));
            }
            // Interior tokens only (skip the first and last of the line).
            for (std::size_t i = 1U; i + 1U < toks.size(); ++i) {
                vocab.insert(toks[i]);
            }
        }
    }
    return vocab;
}

bool compound_hyphen(std::u32string_view prev, std::u32string_view cont, const WordSet& vocab) {
    if (prev.empty()) {
        return false;
    }
    const std::size_t e = prev.size() - 1U;  // step over the trailing hyphen glyph
    std::size_t l = e;
    while (l > 0U && is_ascii_alpha(prev[l - 1U])) {
        --l;
    }
    const std::u32string left = lower_ascii(prev.substr(l, e - l));
    std::size_t r = 0U;
    while (r < cont.size() && is_ascii_alpha(cont[r])) {
        ++r;
    }
    const std::u32string right = lower_ascii(cont.substr(0U, r));

    // The continuation itself opens a hyphenated compound ("to-German"): the wrap
    // fell inside a multi-part compound ("English-" / "to-German").
    const bool cont_compound = r > 0U && r + 1U < cont.size() &&
                               (cont[r] == U'-' || cont[r] == kUnicodeHyphenCp) &&
                               is_ascii_alpha(cont[r + 1U]);
    if (left.size() < kMinCompoundPartLen ||
        (right.size() < kMinCompoundPartLen && !cont_compound)) {
        return false;
    }
    if (vocab.find(left) == vocab.end()) {
        return false;
    }
    // Guard against a single word that wrapped at a syllable boundary whose
    // prefix happens to be a real word ("special"+"ization" = "specialization").
    if (vocab.find(left + right) != vocab.end()) {
        return false;
    }
    // Bound Latin/Greek prefixes attach to their stem without a hyphen.
    if (std::binary_search(kBoundPrefixes.begin(), kBoundPrefixes.end(),
                           std::u32string_view{left})) {
        return false;
    }
    return true;
}

bool literal_wrap_hyphen(std::u32string_view prev, std::u32string_view cont,
                         const WordSet& vocab) {
    std::size_t e = prev.size();
    while (e > 0U && (prev[e - 1U] == U'-' || prev[e - 1U] == kUnicodeHyphenCp ||
                      prev[e - 1U] == kSoftHyphenCp)) {
        --e;
    }
    std::size_t b = e;
    while (b > 0U && prev[b - 1U] != U' ') {
        --b;
    }
    if (b == e) {
        return false;
    }
    const std::u32string_view body = prev.substr(b, e - b);
    int depth = 0;
    std::size_t frag = std::u32string_view::npos;
    for (std::size_t i = 0U; i < body.size(); ++i) {
        const char32_t c = body[i];
        if (c == U'{') {
            ++depth;
        } else if (c == U'}') {
            --depth;
        } else if (c == U'-' || c == kUnicodeHyphenCp) {
            frag = i + 1U;
        }
    }
    if (depth > 0) {
        return true;
    }
    if (frag == std::u32string_view::npos) {
        return false;
    }
    for (std::size_t i = frag; i < body.size(); ++i) {
        if (!is_ascii_alpha(body[i]) && !is_ascii_digit(body[i])) {
            return false;
        }
    }
    const std::u32string left = lower_ascii(body.substr(frag));
    std::size_t r = 0U;
    while (r < cont.size() && is_ascii_alpha(cont[r])) {
        ++r;
    }
    const std::u32string right = lower_ascii(cont.substr(0U, r));
    if (!left.empty() && !right.empty() && vocab.find(left + right) != vocab.end()) {
        return false;
    }
    return true;
}

bool starts_new_url(std::u32string_view tok) noexcept {
    if (tok.size() >= 4U && tok.substr(0U, 4U) == U"www.") {
        return true;
    }
    const std::size_t s = tok.find(U"://");
    if (s == std::u32string_view::npos || s == 0U || s > kMaxUrlSchemeLen) {
        return false;
    }
    for (std::size_t i = 0U; i < s; ++i) {
        if (!is_ascii_alpha(tok[i])) {
            return false;
        }
    }
    return true;
}

bool url_wrap(std::u32string_view prev, std::u32string_view next) noexcept {
    const char32_t first = next.empty() ? U'\0' : next.front();
    std::size_t e = prev.size();
    while (e > 0U && prev[e - 1U] == U' ') {
        --e;
    }
    std::size_t b = e;
    while (b > 0U && prev[b - 1U] != U' ') {
        --b;
    }
    if (e == b) {
        return false;
    }
    const std::u32string_view tok = prev.substr(b, e - b);
    const bool is_url = tok.find(U"://") != std::u32string_view::npos ||
                        (tok.size() >= 4U && tok.substr(0U, 4U) == U"www.");
    if (!is_url || starts_new_url(next)) {
        return false;
    }
    const char32_t last = prev[e - 1U];
    if (last == U'.' || last == U'!' || last == U'?' || last == U',' || last == U':' ||
        last == U';' || last == U')') {
        return false;
    }
    const bool sep = last == U'/' || last == U'-' || last == U'_' || last == U'.' ||
                     last == U'=' || last == U'?' || last == U'&' || last == U'#' ||
                     last == U'%' || last == U'+' || last == U'~';
    if (is_ascii_upper(first) && !sep) {
        return false;
    }
    return is_ascii_alpha(first) || is_ascii_digit(first) || first == U'/' || first == U'.' ||
           first == U'-' || first == U'_' || first == U'~' || first == U'#' || first == U'?' ||
           first == U'=' || first == U'&' || first == U'%' || first == U'+';
}

bool same_mergeable_style(const StyledRun& a, const StyledRun& b) noexcept {
    return !a.raw && !b.raw && a.bold == b.bold && a.italic == b.italic && a.mono == b.mono &&
           a.math == b.math && a.strikethrough == b.strikethrough && a.link_url == b.link_url;
}

std::vector<StyledRun> merge_block_runs(const Block& block, const WordSet& vocab) {
    std::vector<StyledRun> merged;

    const auto append_run = [&](const StyledRun& run) {
        if (run.text.empty()) {
            return;
        }
        if (!merged.empty() && same_mergeable_style(merged.back(), run)) {
            merged.back().text += run.text;
            return;
        }
        merged.push_back(run);
    };

    for (const Line& line : block.lines) {
        if (line.runs.empty()) {
            continue;
        }
        if (!merged.empty()) {
            std::u32string& prev = merged.back().text;
            const char32_t first = line.plain.empty() ? U'\0' : line.plain.front();
            bool joined = false;
            while (!prev.empty() && prev.back() == U' ') {
                prev.pop_back();
            }
            if (!prev.empty()) {
                const char32_t last = prev.back();
                const bool lower_wrap =
                    last == kSoftHyphenCp ||
                    ((last == U'-' || last == kUnicodeHyphenCp) && is_ascii_lower(first));
                if (lower_wrap) {
                    if (compound_hyphen(prev, line.plain, vocab) ||
                        literal_wrap_hyphen(prev, line.plain, vocab)) {
                        if (last == kSoftHyphenCp || last == kUnicodeHyphenCp) {
                            prev.pop_back();
                            prev += U'-';
                        }
                    } else {
                        prev.pop_back();
                    }
                    joined = true;
                } else if ((last == U'-' || last == kUnicodeHyphenCp) && is_ascii_upper(first)) {
                    if (last == kUnicodeHyphenCp) {
                        prev.pop_back();
                        prev += U'-';
                    }
                    joined = true;
                } else if (url_wrap(prev, line.plain)) {
                    joined = true;
                }
            }
            if (!joined) {
                prev += U' ';
            }
            if (merged.back().text.empty()) {
                merged.pop_back();
            }
        }
        for (const StyledRun& run : line.runs) {
            append_run(run);
        }
    }

    // Remove remaining soft hyphens in place without reallocating clean runs.
    for (StyledRun& run : merged) {
        std::erase(run.text, kSoftHyphenCp);
    }
    std::erase_if(merged, [](const StyledRun& r) { return r.text.empty(); });

    // Letter-free fixed-pitch runs (numbers, punctuation, currency/triangle symbols)
    // inside prose are font flag noise rather than inline code; demote and re-merge.
    for (StyledRun& run : merged) {
        if (!run.mono) {
            continue;
        }
        const bool has_letter = std::any_of(run.text.begin(), run.text.end(), is_word_letter_cp);
        if (!has_letter) {
            run.mono = false;
        }
    }

    std::vector<StyledRun> repacked;
    repacked.reserve(merged.size());
    for (StyledRun& run : merged) {
        if (!repacked.empty() && same_mergeable_style(repacked.back(), run)) {
            repacked.back().text += run.text;
            continue;
        }
        repacked.push_back(std::move(run));
    }
    return repacked;
}

}  // namespace pdf2md::markdown
