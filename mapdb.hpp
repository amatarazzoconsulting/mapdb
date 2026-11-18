#pragma once
/*
MapDB - Memory-Mapped Zero-Copy Database Engine
Copyright (C) 2026 Anthony Matarazzo

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without
limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

--------------------------------------------------------------------------------
Single-header, header-only library. Requires C++17 and a POSIX platform (Linux,
macOS, BSD) because storage is built on mmap(2).

Design notes that matter when you use this header:

  * Records are stored verbatim in a memory-mapped file, so a record type must be
    trivially copyable (no std::string, no std::vector, no virtual functions).
    Use mapdb::FixedString<N> for text fields, or plain fixed-size char arrays.
  * Pointer views point straight into the mapping. They stay valid until the
    process reopens the table or another thread inserts into it.
  * Indexes (hash + trie) live in RAM and are rebuilt automatically when a table
    is opened, by scanning the file once. They are not serialised to disk.
  * Compression codecs (NibbleCompressor, RLE, Delta) are provided as standalone,
    reversible utilities. They are not applied transparently inside Storage.
--------------------------------------------------------------------------------
*/

#include <algorithm>
#include <any>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#error "mapdb.hpp currently targets POSIX (mmap). Windows needs a CreateFileMapping port."
#endif

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mapdb {

/* ============================================================
   FIXED SIZE TEXT
   ============================================================ */

/**
 * @brief Fixed-capacity, trivially copyable string usable inside a mapped record.
 *
 * Assignment truncates to N-1 characters and always keeps a terminating '\0',
 * so the raw bytes are safe to memcpy into a file and read back later.
 */
template <size_t N>
struct FixedString {
    static_assert(N > 0, "FixedString needs a non-zero capacity");
    char buf[N]{};

    FixedString() = default;
    FixedString(const char* s) { assign(s); }                       // NOLINT(google-explicit-constructor)
    FixedString(std::string_view s) { assign(s); }                  // NOLINT(google-explicit-constructor)

    FixedString& operator=(const char* s) { assign(s); return *this; }
    FixedString& operator=(std::string_view s) { assign(s); return *this; }

    void assign(std::string_view s) {
        size_t n = std::min(s.size(), N - 1);
        if (n) std::memcpy(buf, s.data(), n);
        std::memset(buf + n, 0, N - n);
    }
    void assign(const char* s) { assign(s ? std::string_view(s) : std::string_view()); }

    const char* c_str() const { return buf; }
    size_t size() const { return sv().size(); }
    bool empty() const { return buf[0] == '\0'; }
    std::string_view sv() const { return std::string_view(buf, ::strnlen(buf, N)); }
    operator std::string_view() const { return sv(); }              // NOLINT(google-explicit-constructor)

    bool starts_with(std::string_view p) const {
        return sv().size() >= p.size() && sv().compare(0, p.size(), p) == 0;
    }
    bool contains(std::string_view p) const { return sv().find(p) != std::string_view::npos; }

    bool operator==(const FixedString& o) const { return sv() == o.sv(); }
    bool operator!=(const FixedString& o) const { return sv() != o.sv(); }
    bool operator<(const FixedString& o) const { return sv() < o.sv(); }
    bool operator==(std::string_view o) const { return sv() == o; }
    bool operator!=(std::string_view o) const { return sv() != o; }
};

using Name = FixedString<64>;
using Text = FixedString<256>;

/* ============================================================
   HASHING
   ============================================================ */

/**
 * @brief Hash functor used by every index.
 *
 * Specialised for the two key types std::hash cannot handle on its own:
 * FixedString and std::chrono::time_point. Specialising mapdb::Hash (instead of
 * std::hash) keeps user code and the standard library well defined.
 */
template <typename T>
struct Hash : std::hash<T> {};

template <size_t N>
struct Hash<FixedString<N>> {
    size_t operator()(const FixedString<N>& s) const {
        return std::hash<std::string_view>()(s.sv());
    }
};

template <typename Clock, typename Duration>
struct Hash<std::chrono::time_point<Clock, Duration>> {
    size_t operator()(const std::chrono::time_point<Clock, Duration>& t) const {
        return std::hash<int64_t>()(static_cast<int64_t>(t.time_since_epoch().count()));
    }
};

/* ============================================================
   TIME/DATE UTILITIES
   ============================================================ */

/**
 * @brief Parse "YYYY-MM-DDTHH:MM:SS" (or a custom strftime-style format) as UTC.
 * @throws std::runtime_error when the text does not match the format.
 */
inline std::chrono::system_clock::time_point parse_time(
    const std::string& s, const std::string& fmt = "%Y-%m-%dT%H:%M:%S") {
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, fmt.c_str());
    if (ss.fail()) throw std::runtime_error("mapdb: cannot parse time '" + s + "'");
    std::time_t tt = ::timegm(&tm);
    if (tt == static_cast<std::time_t>(-1)) throw std::runtime_error("mapdb: time out of range '" + s + "'");
    return std::chrono::system_clock::from_time_t(tt);
}

/** @brief Format a time point as UTC text. */
inline std::string format_time(const std::chrono::system_clock::time_point& t,
                               const std::string& fmt = "%Y-%m-%dT%H:%M:%S") {
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    if (!::gmtime_r(&tt, &tm)) throw std::runtime_error("mapdb: gmtime_r failed");
    char buf[128];
    size_t n = std::strftime(buf, sizeof(buf), fmt.c_str(), &tm);
    if (n == 0) throw std::runtime_error("mapdb: strftime failed for format '" + fmt + "'");
    return std::string(buf, n);
}

/* ============================================================
   KEY NORMALISATION (for the trie / prefix index)
   ============================================================ */

inline std::string to_key_string(std::string_view s) { return std::string(s); }
inline std::string to_key_string(const char* s) { return s ? std::string(s) : std::string(); }
template <size_t N>
std::string to_key_string(const FixedString<N>& s) { return std::string(s.sv()); }
template <typename T, typename std::enable_if_t<std::is_arithmetic<T>::value, int> = 0>
std::string to_key_string(T v) {
    std::ostringstream os;
    os << v;
    return os.str();
}
template <typename Clock, typename Duration>
std::string to_key_string(const std::chrono::time_point<Clock, Duration>& t) {
    return format_time(std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(t.time_since_epoch())));
}

/* ============================================================
   MEMORY MAPPED FILE
   ============================================================ */

/**
 * @brief Owns one mmap(2) region. Move-only, closed and unmapped on destruction.
 *
 * If the file already exists and is larger than the requested size, the larger
 * size wins so reopening a table never hides records written by an earlier run.
 */
class MemoryMappedFile {
public:
    MemoryMappedFile() = default;
    MemoryMappedFile(const MemoryMappedFile&) = delete;
    MemoryMappedFile& operator=(const MemoryMappedFile&) = delete;

    void open(const std::string& filename, size_t requested_size) {
        if (data_) throw std::logic_error("mapdb: MemoryMappedFile already open");
        if (requested_size == 0) throw std::invalid_argument("mapdb: mapping size must be > 0");

        fd_ = ::open(filename.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_ < 0)
            throw std::runtime_error("mapdb: cannot open '" + filename + "': " + std::strerror(errno));

        struct stat st{};
        if (::fstat(fd_, &st) != 0) {
            int e = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("mapdb: fstat failed for '" + filename + "': " + std::strerror(e));
        }

        size_t target = std::max(requested_size, static_cast<size_t>(st.st_size));
        if (static_cast<size_t>(st.st_size) < target) {
            if (::ftruncate(fd_, static_cast<off_t>(target)) != 0) {
                int e = errno;
                ::close(fd_);
                fd_ = -1;
                throw std::runtime_error("mapdb: ftruncate failed for '" + filename + "': " + std::strerror(e));
            }
        }

        void* p = ::mmap(nullptr, target, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            int e = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("mapdb: mmap failed for '" + filename + "': " + std::strerror(e));
        }

        path_ = filename;
        data_ = p;
        size_ = target;
    }

    void* data() { return data_; }
    const void* data() const { return data_; }
    size_t size() const { return size_; }
    const std::string& path() const { return path_; }
    bool is_open() const { return data_ != nullptr; }

    /** @brief Force dirty pages out to disk (msync MS_SYNC). */
    void flush() {
        if (!data_) return;
        if (::msync(data_, size_, MS_SYNC) != 0)
            throw std::runtime_error("mapdb: msync failed for '" + path_ + "': " + std::strerror(errno));
    }

    ~MemoryMappedFile() { close(); }

    void close() noexcept {
        if (data_) {
            ::munmap(data_, size_);
            data_ = nullptr;
        }
        if (fd_ != -1) {
            ::close(fd_);
            fd_ = -1;
        }
        size_ = 0;
    }

private:
    void* data_ = nullptr;
    size_t size_ = 0;
    int fd_ = -1;
    std::string path_;
};

/* ============================================================
   INLINE COMPRESSION CODECS
   ============================================================ */

/**
 * @brief Packs two 4-bit values per byte. Values are masked to 4 bits.
 *
 * decompress() needs the original element count because a trailing odd element
 * has no partner; the size is passed explicitly so round trips are exact.
 */
class NibbleCompressor {
public:
    static void compress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        out.clear();
        out.reserve((in.size() + 1) / 2);
        for (size_t i = 0; i < in.size(); i += 2) {
            uint8_t packed = static_cast<uint8_t>((in[i] & 0x0F) << 4);
            if (i + 1 < in.size()) packed |= static_cast<uint8_t>(in[i + 1] & 0x0F);
            out.push_back(packed);
        }
    }

    static void decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, size_t original_size) {
        out.clear();
        out.reserve(original_size);
        for (uint8_t b : in) {
            if (out.size() < original_size) out.push_back(static_cast<uint8_t>((b >> 4) & 0x0F));
            if (out.size() < original_size) out.push_back(static_cast<uint8_t>(b & 0x0F));
        }
    }
};

/**
 * @brief Run-length encoding.
 *
 * Runs are emitted as (value, count) pairs where the count is a full 32-bit
 * unsigned integer, so runs longer than a single byte round trip exactly.
 * Values must be trivially copyable and comparable.
 *
 * @code
 * auto packed   = mapdb::RLE<uint8_t>::compress(values);
 * auto restored = mapdb::RLE<uint8_t>::decompress(packed);
 * @endcode
 */
template <typename T>
class RLE {
    static_assert(std::is_trivially_copyable<T>::value, "RLE requires trivially copyable values");

public:
    struct Run {
        T value;
        uint32_t count;
    };

    static std::vector<Run> compress(const std::vector<T>& in) {
        std::vector<Run> out;
        size_t i = 0;
        while (i < in.size()) {
            const T value = in[i];
            size_t run = 1;
            while (i + run < in.size() && in[i + run] == value && run < 0xFFFFFFFFu) ++run;
            out.push_back(Run{value, static_cast<uint32_t>(run)});
            i += run;
        }
        return out;
    }

    static std::vector<T> decompress(const std::vector<Run>& runs) {
        std::vector<T> out;
        for (const Run& r : runs) {
            if (r.count == 0) throw std::runtime_error("mapdb: RLE stream has a zero-length run");
            out.insert(out.end(), static_cast<size_t>(r.count), r.value);
        }
        return out;
    }
};

/**
 * @brief Delta encoding: first value verbatim, then successive differences.
 *
 * decode() is the exact inverse for any integral type, including unsigned types,
 * because the difference is computed in the same unsigned domain on both sides.
 */
template <typename T>
class Delta {
public:
    static std::vector<T> encode(const std::vector<T>& in) {
        static_assert(std::is_integral<T>::value, "Delta requires an integral type");
        std::vector<T> out;
        if (in.empty()) return out;
        out.reserve(in.size());
        out.push_back(in[0]);
        for (size_t i = 1; i < in.size(); ++i) out.push_back(static_cast<T>(in[i] - in[i - 1]));
        return out;
    }

    static std::vector<T> decode(const std::vector<T>& in) {
        static_assert(std::is_integral<T>::value, "Delta requires an integral type");
        std::vector<T> out;
        if (in.empty()) return out;
        out.reserve(in.size());
        out.push_back(in[0]);
        for (size_t i = 1; i < in.size(); ++i) out.push_back(static_cast<T>(out[i - 1] + in[i]));
        return out;
    }
};

/* ============================================================
   RECORD STORE (the raw mapped bytes)
   ============================================================ */

/**
 * @brief Fixed-size record slots in a memory-mapped file.
 *
 * Layout per slot: [header][record bytes][padding], where the header is aligned
 * to alignof(Record) so the record itself is always correctly aligned. The
 * header carries the record size, which doubles as a validity marker: a slot
 * counts as used only when its header matches sizeof(Record). A freshly created
 * file is zero filled, so scanning stops at the first unused slot and reopening
 * a table resumes appending instead of overwriting.
 */
template <typename Record>
class RecordStore {
    static_assert(std::is_trivially_copyable<Record>::value,
                  "mapdb: Record must be trivially copyable (use FixedString instead of std::string)");

    struct alignas(alignof(Record)) Header {
        uint64_t size;
    };

    static constexpr size_t kAlign = alignof(Record) > alignof(Header) ? alignof(Record) : alignof(Header);
    static constexpr size_t kSlot =
        ((sizeof(Header) + sizeof(Record) + kAlign - 1) / kAlign) * kAlign;

public:
    RecordStore() = default;
    RecordStore(const RecordStore&) = delete;
    RecordStore& operator=(const RecordStore&) = delete;

    void open(const std::string& file, size_t capacity_bytes) {
        mmap_.open(file, capacity_bytes);
        capacity_ = mmap_.size();
        count_ = scan_count();
        offset_ = count_ * kSlot;
    }

    /** @brief Append a record and return its id (the byte offset of its slot). */
    size_t insert(const Record& r) {
        if (offset_ + kSlot > capacity_)
            throw std::runtime_error("mapdb: capacity exceeded in '" + mmap_.path() + "' (" +
                                     std::to_string(capacity_) + " bytes); reopen with a larger size");
        char* base = static_cast<char*>(mmap_.data());
        auto* h = reinterpret_cast<Header*>(base + offset_);
        h->size = sizeof(Record);
        auto* slot = reinterpret_cast<Record*>(h + 1);
        std::memcpy(static_cast<void*>(slot), static_cast<const void*>(&r), sizeof(Record));
        size_t id = offset_;
        offset_ += kSlot;
        ++count_;
        return id;
    }

    Record& get(size_t id) {
        check_id(id);
        return *reinterpret_cast<Record*>(static_cast<char*>(mmap_.data()) + id + sizeof(Header));
    }

    const Record& get(size_t id) const {
        check_id(id);
        return *reinterpret_cast<const Record*>(static_cast<const char*>(mmap_.data()) + id + sizeof(Header));
    }

    size_t count() const { return count_; }
    size_t capacity_bytes() const { return capacity_; }
    size_t bytes_used() const { return offset_; }
    static constexpr size_t slot_size() { return kSlot; }
    const std::string& path() const { return mmap_.path(); }
    void flush() { mmap_.flush(); }

    std::vector<size_t> ids() const {
        std::vector<size_t> out;
        out.reserve(count_);
        for (size_t i = 0; i < count_; ++i) out.push_back(i * kSlot);
        return out;
    }

private:
    size_t scan_count() const {
        const char* base = static_cast<const char*>(mmap_.data());
        size_t n = 0;
        while ((n + 1) * kSlot <= capacity_) {
            auto* h = reinterpret_cast<const Header*>(base + n * kSlot);
            if (h->size != sizeof(Record)) break;
            ++n;
        }
        return n;
    }

    void check_id(size_t id) const {
        if (id % kSlot != 0 || id >= offset_)
            throw std::out_of_range("mapdb: record id " + std::to_string(id) + " is not valid in '" +
                                    mmap_.path() + "'");
    }

    MemoryMappedFile mmap_;
    size_t capacity_ = 0;
    size_t offset_ = 0;
    size_t count_ = 0;
};

/* ============================================================
   RADIX TRIE (prefix index)
   ============================================================ */

/**
 * @brief Character trie that records every id passing through a node, so
 * prefix(key) returns all ids whose normalised key starts with `key`.
 *
 * Children are owned through unique_ptr: no leaks, no manual frees.
 */
class RadixTrie {
    struct Node {
        std::vector<size_t> ids;
        std::unordered_map<char, std::unique_ptr<Node>> children;
    };

public:
    void insert(std::string_view key, size_t id) {
        root_.ids.push_back(id);  // the root holds every id, so prefix("") matches all
        Node* node = &root_;
        for (char c : key) {
            auto& slot = node->children[c];
            if (!slot) slot = std::make_unique<Node>();
            node = slot.get();
            node->ids.push_back(id);
        }
    }

    std::vector<size_t> prefix(std::string_view key) const {
        const Node* node = &root_;
        for (char c : key) {
            auto it = node->children.find(c);
            if (it == node->children.end()) return {};
            node = it->second.get();
        }
        return node->ids;
    }

    void clear() { root_ = Node{}; }

private:
    Node root_;
};

/* ============================================================
   HASH INDEX
   ============================================================ */

template <typename Key, typename Hasher = Hash<Key>>
class HashIndex {
public:
    void insert(const Key& key, size_t id) { map_[key].push_back(id); }

    const std::vector<size_t>& find(const Key& key) const {
        static const std::vector<size_t> empty;
        auto it = map_.find(key);
        return it == map_.end() ? empty : it->second;
    }

    size_t key_count() const { return map_.size(); }
    void clear() { map_.clear(); }

private:
    std::unordered_map<Key, std::vector<size_t>, Hasher> map_;
};

/* ============================================================
   POINTER VIEW
   ============================================================ */

/**
 * @brief Zero-copy result set: a vector of pointers into the mapped records.
 *
 * Iteration yields Record*, so both `for (auto* r : view)` and
 * `for (Record* r : view)` work.
 */
template <typename Record>
class PointerView {
public:
    using value_type = Record*;

    void add(Record* r) { ptrs_.push_back(r); }

    void append(const PointerView& other) {
        ptrs_.insert(ptrs_.end(), other.ptrs_.begin(), other.ptrs_.end());
    }

    size_t size() const { return ptrs_.size(); }
    bool empty() const { return ptrs_.empty(); }
    Record* operator[](size_t i) const { return ptrs_[i]; }
    const std::vector<Record*>& pointers() const { return ptrs_; }

    class iterator {
    public:
        iterator(const PointerView* v, size_t i) : view_(v), i_(i) {}
        Record* operator*() const { return view_->ptrs_[i_]; }
        Record* operator->() const { return view_->ptrs_[i_]; }
        iterator& operator++() { ++i_; return *this; }
        bool operator==(const iterator& o) const { return i_ == o.i_; }
        bool operator!=(const iterator& o) const { return i_ != o.i_; }

    private:
        const PointerView* view_;
        size_t i_;
    };

    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, ptrs_.size()); }

private:
    std::vector<Record*> ptrs_;
};

/* ============================================================
   PARALLEL EXECUTION
   ============================================================ */

/**
 * @brief Split [begin,end) across std::async tasks and call fn(a,b) on each range.
 *
 * Falls back to a serial call for small ranges. `fn` must be safe to run
 * concurrently; `depth` caps the number of spawned tasks at 2^depth.
 */
template <typename Func>
void parallel_for(size_t begin, size_t end, Func fn, size_t grain = 4096, int depth = 4) {
    if (end <= begin) return;
    if (end - begin <= grain || depth <= 0) {
        fn(begin, end);
        return;
    }
    size_t mid = begin + (end - begin) / 2;
    auto left = std::async(std::launch::async, [&] { parallel_for(begin, mid, fn, grain, depth - 1); });
    parallel_for(mid, end, fn, grain, depth - 1);
    left.get();
}

/* ============================================================
   WILDCARD MATCH
   ============================================================ */

/** @brief Glob match where '*' matches any run of characters and '?' matches one. */
inline bool wildcard_match(std::string_view pat, std::string_view str) {
    constexpr size_t npos = std::string_view::npos;
    size_t p = 0, s = 0, star = npos, mark = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++p;
            ++s;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = s;
        } else if (star != npos) {
            p = star + 1;
            s = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

/* ============================================================
   STORAGE (the table)
   ============================================================ */

/**
 * @brief A typed table: mapped records plus optional hash and trie indexes.
 *
 * @code
 * Storage<Customer> customers("customers.db");
 * customers.addHashIndex(&Customer::id);
 * customers.addTrieIndex(&Customer::name);
 * customers.insert({1, "Alice Smith", "alice@example.com", now});
 * for (auto* c : customers.find(1))  std::cout << c->name.c_str();
 * for (auto* c : customers.view([](const Customer& x){ return x.name.starts_with("A"); }))
 *     std::cout << c->name.c_str();
 * @endcode
 */
template <typename Record>
class Storage {
public:
    static constexpr size_t kDefaultCapacity = size_t(64) << 20;  // 64 MiB

    explicit Storage(const std::string& file, size_t capacity_bytes = kDefaultCapacity) {
        open(file, capacity_bytes);
    }

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    void open(const std::string& file, size_t capacity_bytes = kDefaultCapacity) {
        store_.open(file, capacity_bytes);
    }

    /* ---- indexes ---- */

    /** @brief Index a field for exact lookups. Also usable on time_point fields. */
    template <typename Field>
    void addHashIndex(Field Record::*member) {
        auto index = std::make_unique<HashIndexImpl<Field>>(member);
        for (size_t id : store_.ids()) index->insert(store_.get(id), id);
        hash_indexes_.push_back(std::move(index));
    }

    /** @brief Index a field for prefix lookups (string, FixedString, numeric, time). */
    template <typename Field>
    void addTrieIndex(Field Record::*member) {
        auto index = std::make_unique<TrieIndexImpl<Field>>(member);
        for (size_t id : store_.ids()) index->insert(store_.get(id), id);
        trie_indexes_.push_back(std::move(index));
    }

    /* ---- writes ---- */

    size_t insert(const Record& r) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t id = store_.insert(r);
        const Record& stored = store_.get(id);
        for (auto& idx : hash_indexes_) idx->insert(stored, id);
        for (auto& idx : trie_indexes_) idx->insert(stored, id);
        return id;
    }

    void flush() { store_.flush(); }

    /* ---- reads ---- */

    /** @brief Exact lookup using the first hash index whose key type matches. */
    template <typename Field>
    PointerView<Record> find(const Field& value) {
        const std::any boxed{value};
        for (auto& idx : hash_indexes_) {
            if (idx->key_type() != std::type_index(typeid(Field))) continue;
            PointerView<Record> view;
            for (size_t id : idx->find(boxed)) view.add(&store_.get(id));
            return view;
        }
        throw std::runtime_error("mapdb: find() needs addHashIndex() on a field of this key type");
    }

    /** @brief All records whose trie-indexed key starts with `key`. */
    PointerView<Record> prefix(std::string_view key) {
        if (trie_indexes_.empty())
            throw std::runtime_error("mapdb: prefix() needs addTrieIndex() on the field you search");
        PointerView<Record> view;
        std::unordered_map<size_t, bool> seen;
        for (auto& idx : trie_indexes_) {
            for (size_t id : idx->prefix(key)) {
                if (seen.emplace(id, true).second) view.add(&store_.get(id));
            }
        }
        return view;
    }

    /** @brief Filter every record with a predicate (single threaded). */
    template <typename Predicate>
    PointerView<Record> view(Predicate pred) {
        PointerView<Record> out;
        const size_t n = store_.count();
        for (size_t i = 0; i < n; ++i) {
            Record& r = store_.get(i * RecordStore<Record>::slot_size());
            if (pred(r)) out.add(&r);
        }
        return out;
    }

    /** @brief Same as view() but partitions the scan across worker threads. */
    template <typename Predicate>
    PointerView<Record> parallelView(Predicate pred) {
        PointerView<Record> out;
        std::mutex out_mutex;
        const size_t n = store_.count();
        const size_t stride = RecordStore<Record>::slot_size();
        parallel_for(0, n, [&](size_t a, size_t b) {
            std::vector<Record*> local;
            for (size_t i = a; i < b; ++i) {
                Record& r = store_.get(i * stride);
                if (pred(r)) local.push_back(&r);
            }
            std::lock_guard<std::mutex> lock(out_mutex);
            for (Record* p : local) out.add(p);
        });
        return out;
    }

    /** @brief Documented alias for parallelView(). */
    template <typename Predicate>
    PointerView<Record> parallelQuery(Predicate pred) {
        return parallelView(std::move(pred));
    }

    /** @brief Filter a known set of record ids. */
    template <typename Predicate>
    PointerView<Record> where(const std::vector<size_t>& ids, Predicate pred) {
        PointerView<Record> out;
        for (size_t id : ids) {
            Record& r = store_.get(id);
            if (pred(r)) out.add(&r);
        }
        return out;
    }

    PointerView<Record> all() {
        PointerView<Record> out;
        const size_t n = store_.count();
        for (size_t i = 0; i < n; ++i) out.add(&store_.get(i * RecordStore<Record>::slot_size()));
        return out;
    }

    size_t size() const { return store_.count(); }
    bool empty() const { return store_.count() == 0; }
    size_t capacity_bytes() const { return store_.capacity_bytes(); }
    size_t bytes_used() const { return store_.bytes_used(); }
    const std::string& path() const { return store_.path(); }
    Record& at(size_t id) { return store_.get(id); }
    const Record& at(size_t id) const { return store_.get(id); }

private:
    struct HashIndexBase {
        virtual ~HashIndexBase() = default;
        virtual void insert(const Record& r, size_t id) = 0;
        virtual std::vector<size_t> find(const std::any& value) const = 0;
        virtual std::type_index key_type() const = 0;
    };

    template <typename Field>
    struct HashIndexImpl final : HashIndexBase {
        explicit HashIndexImpl(Field Record::*m) : member(m) {}
        void insert(const Record& r, size_t id) override { index.insert(r.*member, id); }
        std::vector<size_t> find(const std::any& value) const override {
            if (const Field* typed = std::any_cast<Field>(&value)) return index.find(*typed);
            return {};
        }
        std::type_index key_type() const override { return std::type_index(typeid(Field)); }

        Field Record::*member;
        HashIndex<Field> index;
    };

    struct TrieIndexBase {
        virtual ~TrieIndexBase() = default;
        virtual void insert(const Record& r, size_t id) = 0;
        virtual std::vector<size_t> prefix(std::string_view key) const = 0;
    };

    template <typename Field>
    struct TrieIndexImpl final : TrieIndexBase {
        explicit TrieIndexImpl(Field Record::*m) : member(m) {}
        void insert(const Record& r, size_t id) override { trie.insert(to_key_string(r.*member), id); }
        std::vector<size_t> prefix(std::string_view key) const override { return trie.prefix(key); }

        Field Record::*member;
        RadixTrie trie;
    };

    RecordStore<Record> store_;
    std::vector<std::unique_ptr<HashIndexBase>> hash_indexes_;
    std::vector<std::unique_ptr<TrieIndexBase>> trie_indexes_;
    std::mutex mutex_;
};

/* ============================================================
   JOIN ENGINE
   ============================================================ */

/**
 * @brief Hash join: build a hash of the left table on its key, probe with the right.
 *
 * Only matching key pairs reach the predicate, so the predicate never sees a
 * cross product. Complexity is O(n + m) plus the cost of the predicate.
 */
template <typename A, typename B, typename KeyA, typename KeyB, typename Predicate>
std::vector<std::pair<A*, B*>> join(Storage<A>& left, Storage<B>& right, KeyA A::*key_a, KeyB B::*key_b,
                                    Predicate pred) {
    static_assert(std::is_convertible<KeyB, KeyA>::value,
                  "mapdb::join needs a right-hand key convertible to the left-hand key type");

    std::unordered_map<KeyA, std::vector<A*>, Hash<KeyA>> buckets;
    for (A* a : left.all()) buckets[a->*key_a].push_back(a);

    std::vector<std::pair<A*, B*>> out;
    for (B* b : right.all()) {
        auto it = buckets.find(static_cast<KeyA>(b->*key_b));
        if (it == buckets.end()) continue;
        for (A* a : it->second) {
            if (pred(a, b)) out.emplace_back(a, b);
        }
    }
    return out;
}

/** @brief Join without a filter: every key-matched pair. */
template <typename A, typename B, typename KeyA, typename KeyB>
std::vector<std::pair<A*, B*>> join(Storage<A>& left, Storage<B>& right, KeyA A::*key_a, KeyB B::*key_b) {
    return join(left, right, key_a, key_b, [](const A*, const B*) { return true; });
}

/* ============================================================
   QUERY AST (predicate holder used by Storage::query)
   ============================================================ */

/**
 * @brief Minimal typed query object: holds a predicate and applies it to a table.
 *
 * `QueryAST<Customer>{}.where(...)` reads like the SQL-ish form used in the docs
 * while staying type safe.
 */
template <typename Record>
struct QueryAST {
    std::function<bool(const Record&)> predicate;

    QueryAST& where(std::function<bool(const Record&)> pred) {
        auto previous = predicate;
        predicate = [previous, pred](const Record& r) {
            return (previous ? previous(r) : true) && pred(r);
        };
        return *this;
    }
};

template <typename Record>
PointerView<Record> query(Storage<Record>& table, const QueryAST<Record>& ast) {
    if (!ast.predicate) return table.all();
    return table.view(ast.predicate);
}

}  // namespace mapdb
