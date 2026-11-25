#pragma once

#include "pdf2md/document_model.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pdf2md::markdown {

using WordSet = std::unordered_set<std::u32string>;

WordSet build_document_vocab(const Document& doc);

bool compound_hyphen(std::u32string_view prev, std::u32string_view cont, const WordSet& vocab);
bool literal_wrap_hyphen(std::u32string_view prev, std::u32string_view cont,
                         const WordSet& vocab);

bool starts_new_url(std::u32string_view tok) noexcept;
bool url_wrap(std::u32string_view prev, std::u32string_view next) noexcept;

bool same_mergeable_style(const StyledRun& a, const StyledRun& b) noexcept;

// Joins the lines of a block into one run sequence, applying hyphenation and
// URL wrap repair at line boundaries, demoting stray fixed-pitch noise, and
// merging adjacent same-style runs.
std::vector<StyledRun> merge_block_runs(const Block& block, const WordSet& vocab);

}  // namespace pdf2md::markdown
