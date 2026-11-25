#pragma once

#include "pdf2md/document_model.h"

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pdf2md::layout {

struct Emit {
    std::size_t idx = 0;
    bool space_before = false;
};

int script_role(const Line& line, const CharInfo& c) noexcept;
std::vector<Emit> plan_line(Line& line);
void repair_token_spaces(const Line& line, std::vector<Emit>& emits);
void normalize_inline_punct(Line& line);
bool line_needs_math(const Line& line) noexcept;
void render_plain(Line& line, std::span<const Emit> emits);

std::u32string math_to_latex(std::vector<const CharInfo*> gl, double axis, bool top_level,
                             int depth = 0, std::span<const RuleSegment> rules = {});

inline std::u32string math_to_latex(std::vector<const CharInfo*> gl, double axis, bool top_level,
                                    std::span<const RuleSegment> rules) {
    return math_to_latex(std::move(gl), axis, top_level, 0, rules);
}

void render_math(Line& line, std::span<const Emit> emits);
void rewrite_footnote_refs(Line& line);

void build_runs(Line& line);
std::vector<Line> build_lines(std::span<const CharInfo* const> chars);

}  // namespace pdf2md::layout
