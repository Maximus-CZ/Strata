// Persistent prefix snapshots (docs/DETAILS.md): the state at the end of a client's system prompt, so a new
// conversation that starts with it reads only the rest.  One file per prefix on disk; the most recently used
// ones also as RAM images.  When RAM is short, saving and restoring stream between the session and the file
// through one bounce buffer.  Single session (no layer split), not thread safe.
#pragma once

#include "strata/core/conversation_snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace strata::core {

class PrefixStore {
public:
    struct Options {
        std::string dir;        // empty: disabled
        std::string identity;   // engine build and model; a file with another one is deleted
        size_t ram_budget = 0, disk_budget = 0, min_free = 0;
        size_t bounce = 64u << 20;
        size_t family_tokens = 1024;   // local patch: snapshots sharing this many first tokens are one family
    };
    struct Match {
        uint64_t key = 0;
        int64_t tokens = 0;
    };

    // Scans the directory: files of another identity, unreadable or partial ones are deleted.
    bool open(const Options& options, std::string& log, std::string& error);
    bool enabled() const { return !o_.dir.empty(); }
    // the pictures of `imgs` that start below `len` tokens: what a snapshot of `len` tokens must have recorded
    static std::vector<ConversationImageKey> imgs_below(const std::vector<ConversationImageKey>& imgs, size_t len) {
        std::vector<ConversationImageKey> v;
        for (const auto& k : imgs) if (k.start >= 0 && (size_t) k.start < len) v.push_back(k);
        return v;
    }
    // the longest prefix of `prompt` (shorter than it: its last token is always read).  local patch: pictures count -
    // a snapshot matches only when every picture in its range is the same picture (content hash) at the same place;
    // the <|image_pad|> tokens alone are equal for any two pictures of one size.
    template<class Token> Match best(const std::vector<Token>& prompt, const std::vector<ConversationImageKey>& imgs,
                                     bool cvec) const {
        Match m;
        for (const auto& e : entries_)
            if (e.cvec == cvec && (int64_t) e.ids.size() > m.tokens && e.ids.size() < prompt.size() &&
                std::equal(e.ids.begin(), e.ids.end(), prompt.begin()) && imgs_below(imgs, e.ids.size()) == e.imgs)
                m = {e.key, (int64_t) e.ids.size()};
        return m;
    }
    bool contains(const std::vector<int32_t>& ids, const std::vector<ConversationImageKey>& imgs, bool cvec) const;
    // local patch (conversation end points on disk): drops the saved snapshots that are a strict prefix of `ids`
    // (pictures included) and longer than `above` tokens - the older end points of the same conversation; the
    // system-prompt root (at most `above` tokens) stays, and so does one that a newer snapshot is stored on
    // (a delta's base).  Returns how many were dropped.
    size_t drop_superseded(const std::vector<int32_t>& ids, const std::vector<ConversationImageKey>& imgs, size_t above,
                           bool cvec);
    // Saves the session's state at `root`, a checkpoint of what the session holds, to a new file.  local patch
    // (delta end points): when a saved snapshot shares most of root's tokens and its bytes match the session's
    // (sampled), the file holds only the bytes past them and links to it - the chain is read back on restore.
    bool capture(const ConversationCheckpoint& root, bool cvec, const SessionState& session, const ModelGeometry& g,
                 const QsaState& draft, std::string& log, std::string& error);
    // local patch (2026-10-21): the same file from a PARKED conversation (the RAM cache is letting it go) - `root`
    // is one of its checkpoints, the bytes come from its K/V copies; the session only gives the layout.
    bool capture_image(const ConversationCheckpoint& root, const SavedConversation& image, const SessionState& session,
                       const ModelGeometry& g, const QsaState& draft, std::string& log, std::string& error);
    // `source`: "ram", "disk>ram" (read into the RAM tier first) or "disk" (streamed).  An invalid file is
    // deleted.  transfer_failed leaves the session partly written.
    ConversationRestore restore(uint64_t key, SessionState& session, const ModelGeometry& g, const QsaState& draft,
                                std::string& source, std::string& error);
    size_t size() const { return entries_.size(); }
    size_t ram_bytes() const;
    size_t disk_bytes() const;

private:
    struct Entry {
        uint64_t key = 0;
        std::vector<int32_t> ids;
        std::vector<ConversationImageKey> imgs;   // local patch: the pictures in the snapshot (start + content hash)
        bool cvec = true;
        size_t file_bytes = 0;
        std::shared_ptr<SavedConversation> ram;
        size_t ram_bytes = 0;
        uint64_t used = 0;
        // local patch (delta end points): bytes [0, from[j][i]) of layer j's payload i are the parent's; the
        // file holds the rest, layer by layer, payload by payload, from file offset data_at on
        uint64_t parent = 0, shape = 0, data_at = 0;
        uint64_t family = 0;   // local patch: its first kFamilyTokens tokens - one client and system prompt
        std::vector<ConversationKvSizes> sizes, from;
    };
    std::string path(uint64_t key) const;
    Entry* find(uint64_t key);
    const Entry* find(uint64_t key) const;
    void erase(uint64_t key);
    void trim_disk(size_t incoming);
    bool trim_ram(size_t incoming);
    void touch(uint64_t key);     // the entry and the chain it is stored on: most recently used
    size_t depth(uint64_t key) const;
    // byte ranges of layer j's payload i in [at, at + n), each from the chain entry that holds it
    struct Span { const Entry* e; size_t at, n; };
    bool spans(const Entry& e, size_t j, size_t i, size_t at, size_t n, std::vector<Span>& out) const;
    uint64_t payload_at(const Entry& e, size_t j, size_t i) const;   // file offset of e's bytes of layer j payload i
    using PayloadRead = std::function<bool(size_t, size_t, size_t, size_t, uint8_t*, std::string&)>;
    using PayloadStream = std::function<bool(const std::vector<ConversationKvSizes>&, std::vector<uint8_t>&,
                                             const ConversationWrite&, std::string&)>;
    bool capture_from(const ConversationCheckpoint& root, bool cvec, const SessionState& session, const ModelGeometry& g,
                      const QsaState& draft, const PayloadRead& read, const PayloadStream& stream, std::string& log,
                      std::string& error);
    ConversationRestore restore_chain(const Entry& e, SessionState& session, const ModelGeometry& g,
                                      const QsaState& draft, std::string& source, std::string& error);

    Options o_;
    std::vector<Entry> entries_;
    uint64_t clock_ = 0;
};

} // namespace strata::core
