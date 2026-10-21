#include "strata/core/prefix_store.hpp"
#include "strata/core/conversation_memory.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <new>
#include <system_error>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace strata::core {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// file: magic, identity, cvec, ids, pictures, parent | running state and K/V headers with each payload's delta start
// | K/V payloads past their delta starts (conversation_kv_layout order)
constexpr char kMagic[8] = {'S', 'T', 'R', 'P', 'F', 'X', '0', '3'};   // 02: + picture keys, 03: + deltas 
constexpr size_t kMaxDepth = 24;        // a longer chain is saved whole again
// snapshots that share their first Options::family_tokens tokens are one family (eviction: the largest first)
uint64_t family_of(const std::vector<int32_t>& ids, size_t tokens) {
    uint64_t h = 1469598103934665603ull;
    const size_t n = std::min(ids.size(), tokens);
    for (size_t i = 0; i < n; ++i)
        for (int b = 0; b < 4; ++b) { h ^= (uint8_t) ((uint32_t) ids[i] >> (8 * b)); h *= 1099511628211ull; }
    return h;
}
constexpr size_t kSample = 4096;        // bytes per sampled comparison with the base
constexpr size_t kSamples = 8;          // sampled places per payload

int64_t seek_to(std::FILE* f, uint64_t at) {
#ifdef _WIN32
    return _fseeki64(f, (long long) at, SEEK_SET);
#else
    return fseeko(f, (off_t) at, SEEK_SET);
#endif
}

uint64_t tell(std::FILE* f) {
#ifdef _WIN32
    return (uint64_t) _ftelli64(f);
#else
    return (uint64_t) ftello(f);
#endif
}

uint64_t mix(uint64_t h, uint64_t v) {
    for (int b = 0; b < 8; ++b) { h ^= (uint8_t) (v >> (8 * b)); h *= 1099511628211ull; }
    return h;
}

// what must agree between a delta and its base: geometry, layer range and every layer's K/V format
uint64_t shape_of(const SavedConversation& m) {
    uint64_t h = 1469598103934665603ull;
    for (int64_t v : m.geometry) h = mix(h, (uint64_t) v);
    h = mix(mix(h, (uint64_t) m.layer_lo), (uint64_t) m.layer_hi);
    for (const auto& k : m.kv)
        for (int64_t v : {(int64_t) k.format, k.heads, k.head_dim, k.page_size, k.idx_dim}) h = mix(h, (uint64_t) v);
    return mix(h, m.kv.size());
}

uint64_t key_of(const std::vector<int32_t>& ids, const std::vector<ConversationImageKey>& imgs, bool cvec) {
    uint64_t h = 1469598103934665603ull ^ (cvec ? 1 : 0);
    for (int32_t id : ids)
        for (int b = 0; b < 4; ++b) { h ^= (uint8_t) ((uint32_t) id >> (8 * b)); h *= 1099511628211ull; }
    for (const auto& k : imgs)   // local patch: each picture's place and content hash
        for (uint64_t v : {(uint64_t) k.start, k.hash})
            for (int b = 0; b < 8; ++b) { h ^= (uint8_t) (v >> (8 * b)); h *= 1099511628211ull; }
    return h;
}

struct File {
    std::FILE* f = nullptr;
    uint64_t left = 0;   // bytes a reader may still take: a corrupt length never allocates past the file
    File(const std::string& path, const char* mode) : f(std::fopen(path.c_str(), mode)) {
        std::error_code ec;
        if (f && mode[0] == 'r') left = fs::file_size(path, ec);
    }
    ~File() { if (f) std::fclose(f); }
    bool put(const void* p, size_t n) { return n == 0 || std::fwrite(p, 1, n, f) == n; }
    bool get(void* p, size_t n) {
        if (n > left || (n && std::fread(p, 1, n, f) != n)) return false;
        left -= n;
        return true;
    }
    template<class T> bool put_pod(const T& v) { return put(&v, sizeof v); }
    template<class T> bool get_pod(T& v) { return get(&v, sizeof v); }
    template<class T> bool put_vec(const std::vector<T>& v) {
        return put_pod<uint64_t>(v.size()) && put(v.data(), v.size() * sizeof(T));
    }
    template<class T> bool get_vec(std::vector<T>& v) {
        uint64_t n = 0;
        if (!get_pod(n) || n > left / sizeof(T)) return false;
        v.resize((size_t) n);
        return get(v.data(), v.size() * sizeof(T));
    }
    template<class T> bool skip_vec() {   // the index needs no running state
        uint64_t n = 0;
        if (!get_pod(n) || n > left / sizeof(T) || seek_to(f, tell(f) + n * sizeof(T)) != 0) return false;
        left -= n * sizeof(T);
        return true;
    }
    bool close() {   // flushed to the disk before the rename publishes it
        bool ok = std::fflush(f) == 0;
#ifdef _WIN32
        ok = ok && _commit(_fileno(f)) == 0;
#else
        ok = ok && fsync(fileno(f)) == 0;
#endif
        ok = std::fclose(f) == 0 && ok;
        f = nullptr;
        return ok;
    }
};

bool put_head(File& w, const std::string& identity, const std::vector<int32_t>& ids,
              const std::vector<ConversationImageKey>& imgs, bool cvec, uint64_t parent) {
    return w.put(kMagic, sizeof kMagic) && w.put_pod<uint64_t>(identity.size()) &&
           w.put(identity.data(), identity.size()) && w.put_pod<uint8_t>(cvec ? 1 : 0) && w.put_vec(ids) &&
           w.put_vec(imgs) && w.put_pod(parent);
}

bool get_head(File& r, const std::string& identity, std::vector<int32_t>& ids,
              std::vector<ConversationImageKey>& imgs, bool& cvec, uint64_t& parent) {
    char magic[sizeof kMagic];
    uint64_t n = 0;
    uint8_t c = 0;
    if (!r.get(magic, sizeof magic) || !std::equal(magic, magic + sizeof magic, kMagic) || !r.get_pod(n) ||
        n != identity.size()) return false;
    std::string id(identity.size(), '\0');
    if (!r.get(id.data(), id.size()) || id != identity || !r.get_pod(c) || c > 1 || !r.get_vec(ids) || ids.empty() ||
        !r.get_vec(imgs) || !r.get_pod(parent))
        return false;
    for (size_t i = 0; i < imgs.size(); ++i)   // in order, inside the snapshot
        if (imgs[i].start < 0 || (size_t) imgs[i].start >= ids.size() || (i && imgs[i].start <= imgs[i - 1].start))
            return false;
    cvec = c != 0;
    return true;
}

bool put_meta(File& w, const SavedConversation& m, const std::vector<ConversationKvSizes>& sizes,
              const std::vector<ConversationKvSizes>& from) {
    bool ok = w.put(m.geometry.data(), sizeof m.geometry) && w.put_pod(m.layer_lo) && w.put_pod(m.layer_hi);
    for (const auto* v : {&m.live.gdn, &m.live.ple, &m.live.tails, &m.live.dead, &m.live.block_pos})
        ok = ok && w.put_vec(*v);
    ok = ok && w.put_pod<uint64_t>(m.kv.size());
    for (size_t i = 0; ok && i < m.kv.size(); ++i) {
        const auto& k = m.kv[i];
        ok = w.put_pod<int32_t>(k.format) && w.put_pod(k.cells) && w.put_pod(k.heads) && w.put_pod(k.head_dim) &&
             w.put_pod(k.page_size) && w.put_pod(k.pooled_rows) && w.put_pod(k.idx_dim) &&
             w.put(sizes[i].data(), sizeof sizes[i]) && w.put(from[i].data(), sizeof from[i]);
    }
    return ok;
}

bool get_meta(File& r, SavedConversation& m, std::vector<ConversationKvSizes>& sizes,
              std::vector<ConversationKvSizes>& from, bool skip_state = false) {
    bool ok = r.get(m.geometry.data(), sizeof m.geometry) && r.get_pod(m.layer_lo) && r.get_pod(m.layer_hi);
    for (auto* v : {&m.live.gdn, &m.live.ple, &m.live.tails, &m.live.dead, &m.live.block_pos})
        ok = ok && (skip_state ? r.skip_vec<uint8_t>() : r.get_vec(*v));
    uint64_t layers = 0;
    if (!ok || !r.get_pod(layers) || layers > 4096) return false;
    m.kv.resize((size_t) layers);
    sizes.resize((size_t) layers);
    from.resize((size_t) layers);
    uint64_t payload = 0;
    for (size_t i = 0; i < m.kv.size(); ++i) {
        auto& k = m.kv[i];
        int32_t format = 0;
        if (!r.get_pod(format) || !r.get_pod(k.cells) || !r.get_pod(k.heads) || !r.get_pod(k.head_dim) ||
            !r.get_pod(k.page_size) || !r.get_pod(k.pooled_rows) || !r.get_pod(k.idx_dim) ||
            !r.get(sizes[i].data(), sizeof sizes[i]) || !r.get(from[i].data(), sizeof from[i])) return false;
        k.format = format;
        for (size_t a = 0; a < 5; ++a) {
            if (from[i][a] > sizes[i][a]) return false;
            payload += sizes[i][a] - from[i][a];
        }
    }
    return payload == r.left;   // the payloads, exactly: a partial file restores nothing
}
} // namespace

std::string PrefixStore::path(uint64_t key) const {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.pfx", (unsigned long long) key);
    return (fs::path(o_.dir) / name).string();
}

PrefixStore::Entry* PrefixStore::find(uint64_t key) {
    for (auto& e : entries_) if (e.key == key) return &e;
    return nullptr;
}

const PrefixStore::Entry* PrefixStore::find(uint64_t key) const {
    for (const auto& e : entries_) if (e.key == key) return &e;
    return nullptr;
}

// the entry and every snapshot stored on it (a delta without its base is nothing)
void PrefixStore::erase(uint64_t key) {
    std::error_code ec;
    fs::remove(path(key), ec);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.key == key; }),
                   entries_.end());
    for (;;) {
        const auto child = std::find_if(entries_.begin(), entries_.end(),
                                        [&](const Entry& e) { return e.parent == key; });
        if (child == entries_.end()) break;
        erase(child->key);
    }
}

void PrefixStore::touch(uint64_t key) {
    const uint64_t now = ++clock_;
    std::error_code ec;
    for (size_t hop = 0; key && hop <= kMaxDepth + 1; ++hop) {
        Entry* e = find(key);
        if (!e) break;
        e->used = now;
        fs::last_write_time(path(key), fs::file_time_type::clock::now(), ec);   // the LRU order across restarts
        key = e->parent;
    }
}

size_t PrefixStore::depth(uint64_t key) const {
    size_t d = 0;
    for (const Entry* e = find(key); e && e->parent && d <= kMaxDepth + 1; e = find(e->parent)) ++d;
    return d;
}

uint64_t PrefixStore::payload_at(const Entry& e, size_t j, size_t i) const {
    uint64_t at = e.data_at;
    for (size_t a = 0; a < e.sizes.size(); ++a)
        for (size_t b = 0; b < 5; ++b) {
            if (a == j && b == i) return at;
            at += e.sizes[a][b] - e.from[a][b];
        }
    return at;
}

bool PrefixStore::spans(const Entry& e, size_t j, size_t i, size_t at, size_t n, std::vector<Span>& out) const {
    const Entry* cur = &e;
    for (size_t hop = 0; n; ++hop) {
        if (!cur || hop > kMaxDepth + 1 || j >= cur->sizes.size() || at + n > cur->sizes[j][i]) return false;
        const size_t own = cur->from[j][i];
        if (at + n > own) {   // the tail of the range is this entry's
            const size_t from = std::max(at, own);
            out.push_back({cur, from, at + n - from});
            n = from - at;
        }
        if (n) cur = cur->parent ? find(cur->parent) : nullptr;
    }
    return true;
}

size_t PrefixStore::ram_bytes() const {
    size_t n = 0;
    for (const auto& e : entries_) n += e.ram_bytes;
    return n;
}

size_t PrefixStore::disk_bytes() const {
    size_t n = 0;
    for (const auto& e : entries_) n += e.file_bytes;
    return n;
}

// least recently used first, until `incoming` more fits the budget
// local patch (2026-10-18): the least recently used snapshot OF THE LARGEST FAMILY first, not the oldest of all - a
// night of SubSync (38 GB of its agents' end points in 5 h) evicted the Qwen conversation the user came back to in the
// morning. One client now evicts its own snapshots before it touches another's.
void PrefixStore::trim_disk(size_t incoming) {
    while (!entries_.empty() && disk_bytes() + incoming > o_.disk_budget) {
        std::vector<std::pair<uint64_t, size_t>> fam;   // (family, bytes)
        for (const auto& e : entries_) {
            auto it = std::find_if(fam.begin(), fam.end(), [&](const auto& f) { return f.first == e.family; });
            if (it == fam.end()) fam.push_back({e.family, e.file_bytes});
            else it->second += e.file_bytes;
        }
        const uint64_t big = std::max_element(fam.begin(), fam.end(),
                                              [](const auto& a, const auto& b) { return a.second < b.second; })->first;
        const Entry* victim = nullptr;
        for (const auto& e : entries_)
            if (e.family == big && (!victim || e.used < victim->used)) victim = &e;
        erase(victim->key);
    }
}

bool PrefixStore::trim_ram(size_t incoming) {
    if (incoming > o_.ram_budget) return false;
    while (ram_bytes() + incoming > o_.ram_budget) {
        Entry* oldest = nullptr;
        for (auto& e : entries_)
            if (e.ram && (!oldest || e.used < oldest->used)) oldest = &e;
        oldest->ram.reset();
        oldest->ram_bytes = 0;
    }
    return true;
}

bool PrefixStore::open(const Options& options, std::string& log, std::string& error) {
    o_ = options;
    entries_.clear();
    if (!enabled()) return true;
    std::error_code ec;
    fs::create_directories(o_.dir, ec);
    if (ec || !fs::is_directory(o_.dir, ec)) {
        error = "prefix cache: cannot use directory " + o_.dir;
        o_.dir.clear();
        return false;
    }
    struct Found { Entry e; fs::file_time_type t; };
    std::vector<Found> found;
    size_t dropped = 0;
    for (const auto& d : fs::directory_iterator(o_.dir, ec)) {
        const auto p = d.path();
        if (p.extension() == ".tmp") { fs::remove(p, ec); ++dropped; continue; }
        if (p.extension() != ".pfx") continue;
        Entry e;
        bool ok = false;
        {
            File r(p.string(), "rb");
            SavedConversation m;
            ok = r.f && get_head(r, o_.identity, e.ids, e.imgs, e.cvec, e.parent) &&
                 get_meta(r, m, e.sizes, e.from, true);
            if (ok) { e.data_at = tell(r.f); e.shape = shape_of(m); }
        }
        e.key = key_of(e.ids, e.imgs, e.cvec);
        e.family = family_of(e.ids, o_.family_tokens);
        if (!ok || p.filename().string() != fs::path(path(e.key)).filename().string()) {
            fs::remove(p, ec);
            ++dropped;
            continue;
        }
        e.file_bytes = (size_t) fs::file_size(p, ec);
        found.push_back({std::move(e), fs::last_write_time(p, ec)});
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.t < b.t; });
    for (auto& f : found) {
        f.e.used = ++clock_;
        entries_.push_back(std::move(f.e));
    }
    // a delta whose base is gone, of another shape or too short for it, or a chain too long (a loop) is dropped
    for (bool again = true; again;) {
        again = false;
        for (const auto& e : entries_) {
            if (!e.parent) continue;
            const Entry* b = find(e.parent);
            bool ok = b && b->cvec == e.cvec && b->shape == e.shape && b->sizes.size() == e.sizes.size() &&
                      depth(e.key) <= kMaxDepth;
            for (size_t j = 0; ok && j < e.sizes.size(); ++j)
                for (size_t i = 0; i < 5; ++i) ok = ok && e.from[j][i] <= b->sizes[j][i];
            if (!ok) { erase(e.key); ++dropped; again = true; break; }
        }
    }
    trim_disk(0);
    char text[160];
    std::snprintf(text, sizeof text, "%zu snapshots (%.1f MB) in %s, %zu other files deleted", entries_.size(),
                  (double) disk_bytes() / 1e6, o_.dir.c_str(), dropped);
    log = text;
    return true;
}

bool PrefixStore::contains(const std::vector<int32_t>& ids, const std::vector<ConversationImageKey>& imgs,
                           bool cvec) const {
    for (const auto& e : entries_) if (e.cvec == cvec && e.ids == ids && e.imgs == imgs) return true;
    return false;
}

size_t PrefixStore::drop_superseded(const std::vector<int32_t>& ids, const std::vector<ConversationImageKey>& imgs,
                                   size_t above, bool cvec) {
    size_t gone = 0;
    for (bool again = true; again;) {   // a base is dropped once nothing is stored on it any more
        again = false;
        for (const auto& e : entries_)
            if (e.cvec == cvec && e.ids.size() > above && e.ids.size() < ids.size() &&
                std::equal(e.ids.begin(), e.ids.end(), ids.begin()) && imgs_below(imgs, e.ids.size()) == e.imgs &&
                std::none_of(entries_.begin(), entries_.end(), [&](const Entry& c) { return c.parent == e.key; })) {
                erase(e.key);
                ++gone;
                again = true;
                break;
            }
    }
    return gone;
}

bool PrefixStore::capture(const ConversationCheckpoint& root, bool cvec, const SessionState& ss,
                          const ModelGeometry& g, const QsaState& draft, std::string& log, std::string& error) {
    const int64_t upto = (int64_t) root.ids.size();
    return capture_from(root, cvec, ss, g, draft,
        [&](size_t j, size_t i, size_t at, size_t n, uint8_t* out, std::string& e) {
            return conversation_prefix_read(ss, g, draft, upto, j, i, at, n, out, e); },
        [&](const std::vector<ConversationKvSizes>& from, std::vector<uint8_t>& bounce, const ConversationWrite& w,
            std::string& e) { return conversation_prefix_save_stream_from(ss, g, draft, upto, from, bounce, w, e); },
        log, error);
}

bool PrefixStore::capture_image(const ConversationCheckpoint& root, const SavedConversation& image,
                                const SessionState& ss, const ModelGeometry& g, const QsaState& draft, std::string& log,
                                std::string& error) {
    SavedConversation meta;                        // the payload sizes of this prefix (the layout is the session's)
    std::vector<ConversationKvSizes> sizes;
    if (!enabled()) return true;
    if (!conversation_prefix_header(meta, sizes, root, image.cvec, ss, g, draft, error)) return false;
    return capture_from(root, image.cvec, ss, g, draft,
        [&](size_t j, size_t i, size_t at, size_t n, uint8_t* out, std::string& e) {
            return conversation_prefix_read_image(image, sizes, j, i, at, n, out, e); },
        [&](const std::vector<ConversationKvSizes>& from, std::vector<uint8_t>&, const ConversationWrite& w,
            std::string& e) { return conversation_prefix_save_stream_image(image, sizes, from, w, e); },
        log, error);
}

bool PrefixStore::capture_from(const ConversationCheckpoint& root, bool cvec, const SessionState& ss,
                               const ModelGeometry& g, const QsaState& draft, const PayloadRead& read,
                               const PayloadStream& stream, std::string& log, std::string& error) {
    if (!enabled() || contains(root.ids, root.imgs, cvec)) return true;
    const auto t0 = Clock::now();
    SavedConversation meta;
    std::vector<ConversationKvSizes> sizes;
    if (!conversation_prefix_header(meta, sizes, root, cvec, ss, g, draft, error)) return false;
    size_t payload = 0;
    for (const auto& z : sizes) for (size_t n : z) payload += n;
    const size_t whole = root.bytes() + payload;
    if (whole > o_.disk_budget) {
        log = "skipped: larger than the disk budget";
        return true;
    }
    const uint64_t key = key_of(root.ids, root.imgs, cvec);
    if (find(key)) erase(key);   // a hash collision: the newer prefix replaces it
    const uint64_t shape = shape_of(meta);
    const int64_t upto = (int64_t) root.ids.size();

    // local patch (delta end points): the base is the saved snapshot sharing the most tokens (pictures included)
    // - usually this conversation's previous end point.  Its bytes must be the session's: equal tokens computed in
    // another session differ in the last bits, so sampled places of every payload are compared first.
    uint64_t parent = 0;
    int64_t shared = 0;
    std::vector<ConversationKvSizes> from(sizes.size(), ConversationKvSizes{});
    {
        std::vector<std::pair<int64_t, const Entry*>> bases;   // the most shared tokens first
        for (const auto& e : entries_) {
            if (e.cvec != cvec || e.shape != shape || e.sizes.size() != sizes.size() || depth(e.key) >= kMaxDepth)
                continue;
            int64_t c = 0;
            const size_t lim = std::min(e.ids.size(), root.ids.size());
            while ((size_t) c < lim && e.ids[(size_t) c] == root.ids[(size_t) c]) ++c;
            const auto a = imgs_below(root.imgs, (size_t) c), b = imgs_below(e.imgs, (size_t) c);
            for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
                if (i >= a.size() || i >= b.size() || !(a[i] == b[i])) {
                    c = std::min<int64_t>(c, std::min(i < a.size() ? a[i].start : c, i < b.size() ? b[i].start : c));
                    break;
                }
            if (c > 0) bases.push_back({c, &e});
        }
        // a snapshot that is wholly a prefix of this one (its system-prompt root, its own earlier end points) first,
        // most shared tokens first; then the rest.  A sibling (another question on the same root) shares a few more
        // tokens, but those cells were computed in another session and fail the byte check (2026-10-16: SubSync's
        // whole-file questions tried three siblings and saved 855 MB whole each time, the root never tried).
        std::stable_sort(bases.begin(), bases.end(), [](const auto& x, const auto& y) {
            const bool px = (int64_t) x.second->ids.size() == x.first, py = (int64_t) y.second->ids.size() == y.first;
            return px != py ? px : x.first > y.first;
        });
        // the closest one may still hold cells computed in another session (sampled bytes differ): the next is tried
        for (size_t tried = 0; tried < bases.size() && tried < 3 && !parent; ++tried) {
            const Entry* base = bases[tried].second;
            shared = bases[tried].first;
            std::vector<ConversationKvSizes> f;
            conversation_prefix_unchanged(meta, sizes, shared - 1, f);
            size_t kept = 0;
            bool ok = base != nullptr;
            for (size_t j = 0; ok && j < sizes.size(); ++j)
                for (size_t i = 0; i < 5; ++i) {
                    ok = ok && f[j][i] <= base->sizes[j][i];
                    kept += f[j][i];
                }
            ok = ok && kept * 2 >= payload;   // worth a dependency only when it saves at least half
            if (ok) {   // sampled comparison: the start, the end and evenly between, of every payload
                std::vector<uint8_t> mine(kSample), theirs(kSample);
                std::vector<std::unique_ptr<File>> files;
                std::vector<uint64_t> keys;
                auto file_of = [&](uint64_t k) -> std::FILE* {
                    for (size_t x = 0; x < keys.size(); ++x) if (keys[x] == k) return files[x]->f;
                    files.push_back(std::make_unique<File>(path(k), "rb"));
                    keys.push_back(k);
                    return files.back()->f;
                };
                for (size_t j = 0; ok && j < sizes.size(); ++j)
                    for (size_t i = 0; ok && i < 5; ++i) {
                        const size_t len = f[j][i];
                        if (!len) continue;
                        for (size_t k = 0; ok && k <= kSamples; ++k) {
                            const size_t n = std::min(kSample, len);
                            const size_t at = k == kSamples ? len - n : (len - n) / kSamples * k;
                            std::vector<Span> sp;
                            ok = spans(*base, j, i, at, n, sp) &&
                                 read(j, i, at, n, mine.data(), error);
                            size_t got = 0;
                            for (const auto& x : sp) {   // spans come tail first
                                if (!ok) break;
                                std::FILE* r = file_of(x.e->key);
                                ok = r && seek_to(r, payload_at(*x.e, j, i) + (x.at - x.e->from[j][i])) == 0 &&
                                     std::fread(theirs.data() + (x.at - at), 1, x.n, r) == x.n;
                                got += x.n;
                            }
                            ok = ok && got == n && std::equal(mine.begin(), mine.begin() + (ptrdiff_t) n, theirs.begin());
                        }
                    }
                error.clear();
            }
            if (ok) { parent = base->key; from = std::move(f); touch(parent); }
        }
    }
    size_t bytes = whole;
    for (const auto& z : from) for (size_t n : z) bytes -= n;
    trim_disk(bytes);
    if (parent && !find(parent)) {   // the budget took the base: whole after all
        parent = 0;
        from.assign(sizes.size(), ConversationKvSizes{});
        trim_disk(whole);
    }
    const std::string final_path = path(key), tmp = final_path + ".tmp";
    std::error_code ec;
    bool ok = false;
    uint64_t data_at = 0;
    try {
        std::vector<uint8_t> bounce(o_.bounce);
        File w(tmp, "wb");
        ok = w.f && put_head(w, o_.identity, root.ids, root.imgs, cvec, parent) && put_meta(w, meta, sizes, from);
        if (ok) data_at = tell(w.f);
        ok = ok && stream(from, bounce, [&](const uint8_t* p, size_t n) { return w.put(p, n); }, error) &&
             w.close();
        if (!ok && error.empty()) error = "prefix cache: writing " + tmp + " failed";
    } catch (const std::bad_alloc&) {
        error = "prefix cache: no memory for the write buffer";
    }
    if (ok) fs::rename(tmp, final_path, ec);
    if (!ok || ec) {
        fs::remove(tmp, ec);
        if (error.empty()) error = "prefix cache: publishing " + final_path + " failed";
        return false;
    }
    Entry e;
    e.key = key;
    e.ids = root.ids;
    e.imgs = root.imgs;
    e.cvec = cvec;
    e.file_bytes = (size_t) fs::file_size(final_path, ec);
    e.used = ++clock_;
    e.parent = parent;
    e.family = family_of(e.ids, o_.family_tokens);
    e.shape = shape;
    e.data_at = data_at;
    e.sizes = std::move(sizes);
    e.from = std::move(from);
    entries_.push_back(std::move(e));
    char text[224];
    if (parent)
        std::snprintf(text, sizeof text,
                      "saved %zu tokens (%.1f MB, a delta on %lld saved tokens, chain of %zu) in %.1f ms; "
                      "snapshots=%zu disk=%.1f MB",
                      root.ids.size(), (double) entries_.back().file_bytes / 1e6, (long long) shared,
                      depth(key) + 1, std::chrono::duration<double, std::milli>(Clock::now() - t0).count(),
                      entries_.size(), (double) disk_bytes() / 1e6);
    else
        std::snprintf(text, sizeof text, "saved %zu tokens (%.1f MB) in %.1f ms; snapshots=%zu disk=%.1f MB",
                      root.ids.size(), (double) entries_.back().file_bytes / 1e6,
                      std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), entries_.size(),
                      (double) disk_bytes() / 1e6);
    log = text;
    return true;
}

ConversationRestore PrefixStore::restore(uint64_t key, SessionState& ss, const ModelGeometry& g,
                                         const QsaState& draft, std::string& source, std::string& error) {
    Entry* e = find(key);
    if (!e) { error = "prefix cache: no such snapshot"; return ConversationRestore::invalid; }
    touch(key);
    if (e->parent) return restore_chain(*e, ss, g, draft, source, error);
    if (e->ram) {
        source = "ram";
        const auto r = conversation_snapshot_restore(*e->ram, ss, g, draft, error);
        if (r == ConversationRestore::invalid) erase(key);
        return r;
    }
    File r(path(key), "rb");
    auto drop = [&] {   // closed first: Windows does not delete an open file
        if (r.f) std::fclose(r.f);
        r.f = nullptr;
        erase(key);
    };
    SavedConversation m;
    std::vector<ConversationKvSizes> sizes, from;
    bool cvec = true;
    uint64_t parent = 0;
    if (!r.f || !get_head(r, o_.identity, m.live.ids, m.live.imgs, cvec, parent) || !get_meta(r, m, sizes, from) ||
        m.live.ids != e->ids || m.live.imgs != e->imgs || cvec != e->cvec || parent) {
        error = "prefix cache: unreadable snapshot file";
        drop();
        return ConversationRestore::invalid;
    }
    m.cvec = cvec;
    size_t bytes = m.live.bytes() + m.kv.size() * sizeof(ConversationKv);
    for (const auto& z : sizes) for (size_t n : z) bytes += n;
    // the RAM tier when it has room and the machine has the memory; else straight from the file
    if (trim_ram(bytes) && conversation_memory_admit(conversation_available_memory(), bytes, o_.min_free)) {
        try {
            auto image = std::make_shared<SavedConversation>(std::move(m));
            bool ok = true;
            for (size_t i = 0; ok && i < image->kv.size(); ++i) {
                auto& k = image->kv[i];
                ConversationBuffer* parts[5] = {&k.k, &k.v, &k.k_scale, &k.v_scale, &k.pooled};
                for (size_t j = 0; ok && j < 5; ++j) {
                    parts[j]->resize(sizes[i][j]);
                    ok = parts[j]->visit(0, sizes[i][j], [&](uint8_t* p, size_t n, size_t) { return r.get(p, n); });
                }
            }
            if (!ok) {
                error = "prefix cache: reading the snapshot file failed";
                drop();
                return ConversationRestore::invalid;
            }
            source = "disk>ram";
            const auto result = conversation_snapshot_restore(*image, ss, g, draft, error);
            if (result == ConversationRestore::invalid) { drop(); return result; }
            e->ram_bytes = image->bytes();
            e->ram = std::move(image);
            return result;
        } catch (const std::bad_alloc&) {
            error = "prefix cache: no memory to read the snapshot";
            return ConversationRestore::invalid;   // nothing written yet: read the prompt instead
        }
    }
    source = "disk";
    std::vector<uint8_t> bounce;
    try { bounce.resize(o_.bounce); } catch (const std::bad_alloc&) {
        error = "prefix cache: no memory for the read buffer";
        return ConversationRestore::invalid;
    }
    const auto result = conversation_prefix_restore_stream(m, sizes, ss, g, draft, bounce,
                                                           [&](uint8_t* p, size_t n) { return r.get(p, n); }, error);
    if (result == ConversationRestore::invalid) drop();
    return result;
}

// local patch (delta end points): the running state from the entry's own file, the K/V payloads from the chain
ConversationRestore PrefixStore::restore_chain(const Entry& e, SessionState& ss, const ModelGeometry& g,
                                               const QsaState& draft, std::string& source, std::string& error) {
    const uint64_t key = e.key;
    std::vector<std::unique_ptr<File>> files;
    std::vector<uint64_t> keys;
    auto file_of = [&](uint64_t k) -> std::FILE* {
        for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == k) return files[i]->f;
        files.push_back(std::make_unique<File>(path(k), "rb"));
        keys.push_back(k);
        return files.back()->f;
    };
    SavedConversation m;
    std::vector<ConversationKvSizes> sizes, from;
    bool cvec = true;
    uint64_t parent = 0;
    {
        File& r = *(files.push_back(std::make_unique<File>(path(key), "rb")), keys.push_back(key), files.back());
        if (!r.f || !get_head(r, o_.identity, m.live.ids, m.live.imgs, cvec, parent) || !get_meta(r, m, sizes, from) ||
            m.live.ids != e.ids || m.live.imgs != e.imgs || cvec != e.cvec || parent != e.parent ||
            sizes != e.sizes || from != e.from) {
            files.clear();
            error = "prefix cache: unreadable snapshot file";
            erase(key);
            return ConversationRestore::invalid;
        }
    }
    m.cvec = cvec;
    std::vector<ConversationKvPiece> pieces;
    size_t levels = 0;
    bool ok = true;
    std::vector<uint64_t> broken;
    for (size_t j = 0; ok && j < sizes.size(); ++j)
        for (size_t i = 0; ok && i < 5; ++i) {
            std::vector<Span> sp;
            ok = spans(e, j, i, 0, sizes[j][i], sp);
            for (const auto& x : sp) {
                std::FILE* f = file_of(x.e->key);
                std::error_code ec;
                if (!f || fs::file_size(path(x.e->key), ec) != x.e->file_bytes) {   // a base cut short or gone
                    ok = false;
                    broken.push_back(x.e->key);
                    break;
                }
                uint64_t pos = payload_at(*x.e, j, i) + (x.at - x.e->from[j][i]);
                pieces.push_back({j, i, x.at, x.n, [f, pos](uint8_t* p, size_t n) mutable {
                                      const bool good = seek_to(f, pos) == 0 && std::fread(p, 1, n, f) == n;
                                      pos += n;
                                      return good;
                                  }});
            }
        }
    levels = keys.size();
    if (!ok) {
        files.clear();
        error = "prefix cache: a delta's base is unreadable";
        for (uint64_t k : broken) erase(k);
        if (find(key)) erase(key);
        return ConversationRestore::invalid;
    }
    std::vector<uint8_t> bounce;
    try { bounce.resize(o_.bounce); } catch (const std::bad_alloc&) {
        error = "prefix cache: no memory for the read buffer";
        return ConversationRestore::invalid;
    }
    const auto result = conversation_prefix_restore_pieces(m, sizes, ss, g, draft, bounce, pieces, error);
    source = "disk, " + std::to_string(levels) + " files";
    if (result == ConversationRestore::invalid) { files.clear(); erase(key); }
    return result;
}

} // namespace strata::core
