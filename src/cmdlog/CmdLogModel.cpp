#include "CmdLogModel.hpp"

#include <algorithm>

namespace hudcmd {
namespace {
    // A published command line is bounded well below any bus limit before it ever reaches
    // the model; the display truncation on top of this is a separate, configurable width.
    constexpr size_t kMaxSanitisedBytes = 1024;

    // Bytes consumed by the UTF-8 sequence starting at `s[i]`. Invalid lead bytes and
    // truncated sequences advance by one byte so truncation always terminates.
    size_t sequenceLength(const std::string& s, size_t i) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        size_t     want = 1;
        if ((b0 & 0xE0) == 0xC0) want = 2;
        else if ((b0 & 0xF0) == 0xE0) want = 3;
        else if ((b0 & 0xF8) == 0xF0) want = 4;
        if (want == 1 || i + want > s.size())
            return 1;
        for (size_t k = i + 1; k < i + want; ++k)
            if ((static_cast<unsigned char>(s[k]) & 0xC0) != 0x80)
                return 1;
        return want;
    }

    size_t codepointCount(const std::string& s) {
        size_t n = 0;
        for (size_t i = 0; i < s.size(); i += sequenceLength(s, i))
            ++n;
        return n;
    }
}

std::string sanitizeCommand(const std::string& raw) {
    std::string folded;
    folded.reserve(std::min(raw.size(), kMaxSanitisedBytes));
    bool pendingSpace = false;
    for (size_t i = 0; i < raw.size() && folded.size() < kMaxSanitisedBytes; ++i) {
        const auto c = static_cast<unsigned char>(raw[i]);
        // C0 controls, DEL, and ordinary spaces all collapse into a single separator.
        if (c < 0x20 || c == 0x7f || c == ' ') {
            pendingSpace = !folded.empty();
            continue;
        }
        if (pendingSpace) {
            folded.push_back(' ');
            pendingSpace = false;
        }
        folded.push_back(raw[i]);
    }
    // The byte budget can cut mid-sequence; drop that partial tail rather than emit it.
    for (size_t i = 0; i < folded.size();) {
        const auto b0 = static_cast<unsigned char>(folded[i]);
        size_t     want = 1;
        if ((b0 & 0xE0) == 0xC0) want = 2;
        else if ((b0 & 0xF0) == 0xE0) want = 3;
        else if ((b0 & 0xF8) == 0xF0) want = 4;
        if (want > 1 && i + want > folded.size()) {
            folded.resize(i);
            break;
        }
        i += sequenceLength(folded, i);
    }
    return folded;
}

std::string truncateHead(const std::string& text, int maxChars) {
    if (maxChars <= 0)
        return {};
    const size_t want = static_cast<size_t>(maxChars);
    if (codepointCount(text) <= want)
        return text;

    const std::string ellipsis = want > 3 ? "..." : "";
    const size_t      keep     = want - ellipsis.size();
    size_t            i = 0, seen = 0;
    while (i < text.size() && seen < keep) {
        i += sequenceLength(text, i);
        ++seen;
    }
    return text.substr(0, i) + ellipsis;
}

bool CCmdLogModel::publish(const std::string& command, int64_t nowMs) {
    const std::string clean = sanitizeCommand(command);
    if (clean.empty())
        return false;

    // A repeat of the newest live row inside the coalesce window bumps its count instead of
    // pushing a duplicate (the keys model's rule, so a held-down demo does not scroll).
    if (m_config.coalesceMs > 0 && !m_entries.empty() && m_entries.front().text == clean &&
        nowMs - m_entries.front().lastMs <= m_config.coalesceMs) {
        ++m_entries.front().count;
        m_entries.front().lastMs = nowMs;
        return true;
    }

    m_entries.push_front({.text = clean, .count = 1, .lastMs = nowMs});
    while (static_cast<int>(m_entries.size()) > m_config.history)
        m_entries.pop_back();
    return true;
}

bool CCmdLogModel::expire(int64_t nowMs) {
    const size_t before = m_entries.size();
    while (!m_entries.empty() && nowMs - m_entries.back().lastMs >= m_config.ttlMs)
        m_entries.pop_back();
    return m_entries.size() != before;
}

std::vector<std::string> CCmdLogModel::rows() const {
    std::vector<std::string> result;
    result.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
        std::string text = truncateHead(entry.text, m_config.maxChars);
        if (entry.count > 1)
            text += "  x" + std::to_string(entry.count);
        result.push_back(std::move(text));
    }
    return result;
}

std::vector<hud::SLine> CCmdLogModel::lines() const {
    std::vector<hud::SLine> result;
    bool                    newest = true;
    for (auto& row : rows()) {
        result.push_back({std::move(row), newest ? hud::EColor::Accent : hud::EColor::Dim, newest});
        newest = false;
    }
    return result;
}

int64_t CCmdLogModel::nextExpiryMs() const {
    int64_t soonest = -1;
    for (const auto& entry : m_entries) {
        const int64_t deadline = entry.lastMs + m_config.ttlMs;
        if (soonest < 0 || deadline < soonest)
            soonest = deadline;
    }
    return soonest;
}

} // namespace hudcmd
