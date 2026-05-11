#pragma once
/*
MapDB - Memory-Mapped Zero-Copy Database Engine
Copyright (C) 2026 Anthony Matarazzo

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
documentation files (the “Software”), to deal in the Software without restriction, including without 
limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of 
the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT 
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN 
NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <future>
#include <fstream>
#include <cstring>
#include <chrono>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace mapdb {

/* ============================================================
   MEMORY MAPPED FILE
   ============================================================ */

/**
 * @brief MemoryMappedFile manages a memory-mapped file.
 */
class MemoryMappedFile {
    void* data = nullptr;
    size_t size = 0;
    int fd = -1;

public:
    void open(const std::string& filename, size_t file_size) {
        size = file_size;
        fd = ::open(filename.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd < 0) throw std::runtime_error("open failed");

        if (lseek(fd, file_size - 1, SEEK_SET) == -1) throw std::runtime_error("lseek failed");
        if (write(fd, "", 1) != 1) throw std::runtime_error("write failed");

        data = mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) throw std::runtime_error("mmap failed");
    }

    void* get() { return data; }

    ~MemoryMappedFile() {
        if (data) munmap(data, size);
        if (fd != -1) close(fd);
    }
};

/* ============================================================
   INLINE COMPRESSION
   ============================================================ */

class NibbleCompressor {
public:
    static void compress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        out.clear();
        for (size_t i = 0; i < in.size(); i += 2) {
            uint8_t packed = (in[i] & 0x0F) << 4;
            if (i + 1 < in.size()) packed |= (in[i + 1] & 0x0F);
            out.push_back(packed);
        }
    }
    static void decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        out.clear();
        for (auto b : in) {
            out.push_back((b >> 4) & 0x0F);
            out.push_back(b & 0x0F);
        }
    }
};

class RLE {
public:
    template<typename T>
    static std::vector<T> compress(const std::vector<T>& in) {
        std::vector<T> out;
        for (size_t i = 0; i < in.size();) {
            T val = in[i];
            size_t count = 1;
            while (i + count < in.size() && in[i + count] == val) count++;
            out.push_back(val);
            out.push_back(static_cast<T>(count));
            i += count;
        }
        return out;
    }
};

template<typename T>
class Delta {
public:
    static std::vector<T> encode(const std::vector<T>& in) {
        std::vector<T> out;
        if (in.empty()) return out;
        out.push_back(in[0]);
        for (size_t i = 1; i < in.size(); ++i) out.push_back(in[i] - in[i - 1]);
        return out;
    }
};

/* ============================================================
   STORAGE
   ============================================================ */

struct RecordHeader { uint32_t size; };

template<typename Record>
class Storage {
    MemoryMappedFile mmap;
    size_t capacity = 0;
    size_t offset = 0;

public:
    void open(const std::string& file, size_t size) {
        capacity = size;
        mmap.open(file, size);
    }

    size_t insert(const Record& r) {
        size_t total = sizeof(RecordHeader) + sizeof(Record);
        if (offset + total > capacity) throw std::runtime_error("storage full");

        char* base = (char*)mmap.get();
        auto* h = (RecordHeader*)(base + offset);
        h->size = sizeof(Record);
        auto* rec = (Record*)(h + 1);
        memcpy(rec, &r, sizeof(Record));
        size_t id = offset;
        offset += total;
        return id;
    }

    Record& get(size_t id) {
        char* base = (char*)mmap.get();
        auto* h = (RecordHeader*)(base + id);
        return *(Record*)(h + 1);
    }
};

/* ============================================================
   RADIX TRIE
   ============================================================ */

struct RadixNode {
    std::string label;
    std::vector<size_t> ids;
    std::unordered_map<char, RadixNode*> children;
};

class RadixTrie {
    RadixNode root;
public:
    void insert(const std::string& key, size_t id) {
        RadixNode* node = &root;
        for (char c : key) {
            if (!node->children[c]) node->children[c] = new RadixNode();
            node = node->children[c];
            node->ids.push_back(id);
        }
    }
    std::vector<size_t> prefix(const std::string& key) {
        RadixNode* node = &root;
        for (char c : key) { if (!node->children[c]) return {}; node = node->children[c]; }
        return node->ids;
    }
};

/* ============================================================
   HASH INDEX
   ============================================================ */

template<typename Key>
class HashIndex {
    std::unordered_map<Key, std::vector<size_t>> map;
public:
    void insert(const Key& key, size_t id) { map[key].push_back(id); }
    const std::vector<size_t>& find(const Key& key) const {
        static std::vector<size_t> empty;
        auto it = map.find(key);
        return (it != map.end()) ? it->second : empty;
    }
};

namespace std {
template<>
struct hash<std::chrono::system_clock::time_point> {
    size_t operator()(const std::chrono::system_clock::time_point& t) const {
        return std::hash<int64_t>()(t.time_since_epoch().count());
    }
};
}

/* ============================================================
   POINTER VIEW
   ============================================================ */

template<typename Record>
class PointerView {
    std::vector<Record*> ptrs;
public:
    void add(Record* r) { ptrs.push_back(r); }
    class iterator {
        size_t i; PointerView* v;
    public:
        iterator(size_t i_, PointerView* v_) : i(i_), v(v_) {}
        Record& operator*() { return *(v->ptrs[i]); }
        iterator& operator++() { ++i; return *this; }
        bool operator!=(const iterator& o) const { return i != o.i; }
    };
    iterator begin() { return {0,this}; }
    iterator end() { return {ptrs.size(),this}; }
};

/* ============================================================
   TIME/DATE UTILITIES
   ============================================================ */

inline std::chrono::system_clock::time_point parse_time(const std::string& s, const std::string& fmt="%Y-%m-%dT%H:%M:%S") {
    std::tm tm{}; std::istringstream ss(s); ss >> std::get_time(&tm, fmt.c_str());
    if (ss.fail()) throw std::runtime_error("time parse failed");
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

inline std::string format_time(const std::chrono::system_clock::time_point& t, const std::string& fmt="%Y-%m-%dT%H:%M:%S") {
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    char buf[64]; std::strftime(buf,sizeof(buf),fmt.c_str(),std::gmtime(&tt)); return buf;
}

/* ============================================================
   PARALLEL EXECUTION
   ============================================================ */

template<typename Func>
void parallel_for(size_t begin, size_t end, Func fn) {
    if (end-begin <= 512) { fn(begin,end); return; }
    size_t mid=(begin+end)/2;
    auto f1 = std::async(std::launch::async,fn,begin,mid);
    auto f2 = std::async(std::launch::async,fn,mid,end);
    f1.get(); f2.get();
}

/* ============================================================
   WILDCARD MATCH
   ============================================================ */

inline bool wildcard_match(const std::string& pat,const std::string& str) {
    size_t p=0,s=0,star=-1,match=0;
    while(s<str.size()){
        if(p<pat.size()&&(pat[p]=='?'||pat[p]==str[s])){p++;s++;}
        else if(p<pat.size()&&pat[p]=='*'){star=p++;match=s;}
        else if(star!=size_t(-1)){p=star+1;s=++match;}
        else return false;
    }
    while(p<pat.size()&&pat[p]=='*') p++;
    return p==pat.size();
}

/* ============================================================
   AST / QUERY PARSER
   ============================================================ */

enum class ASTNodeType{SELECT,WHERE,JOIN,CONDITION,AND,OR};
struct Condition{std::string field,op,value;};
struct ASTNode{
    ASTNodeType type; Condition condition;
    std::unique_ptr<ASTNode> left; std::unique_ptr<ASTNode> right;
    std::string tableA,tableB,fieldA,fieldB;
};

/* ============================================================
   JOIN ENGINE
   ============================================================ */

template<typename A,typename B,typename KeyA,typename KeyB>
class JoinEngine{
public:
    template<typename GetA,typename GetB>
    static std::vector<std::pair<A*,B*>> join(
        std::vector<A*>& left,std::vector<B*>& right,
        GetA getA,GetB getB)
    {
        std::unordered_map<KeyA,std::vector<A*>> map;
        for(auto* a:left) map[getA(*a)].push_back(a);
        std::vector<std::pair<A*,B*>> result;
        for(auto* b:right){
            auto key=getB(*b);
            if(map.count(key)) for(auto* a:map[key]) result.emplace_back(a,b);
        }
        return result;
    }
};

/* ============================================================
   TABLE
   ============================================================ */

template<typename Record,typename Key,typename Extractor>
class Table{
    Storage<Record> storage;
    HashIndex<Key> hash;
    RadixTrie trie;
    Extractor extractor;
    std::mutex mtx;

public:
    void open(const std::string& file,size_t size){storage.open(file,size);}
    void insert(const Record& r){
        std::lock_guard<std::mutex> lock(mtx);
        size_t id=storage.insert(r);
        Key key=extractor(r);
        hash.insert(key,id);
        trie.insert(key,id);
    }

    PointerView<Record> find(const Key& key){
        PointerView<Record> view;
        for(auto id:hash.find(key)) view.add(&storage.get(id));
        return view;
    }

    PointerView<Record> prefix(const std::string& k){
        PointerView<Record> view;
        for(auto id:trie.prefix(k)) view.add(&storage.get(id));
        return view;
    }

    template<typename Predicate>
    PointerView<Record> where(Predicate pred,const std::vector<size_t>& ids){
        PointerView<Record> view;
        std::mutex out_mtx;
        parallel_for(0,ids.size(),[&](size_t a,size_t b){
            PointerView<Record> local;
            for(size_t i=a;i<b;i++){
                auto& r=storage.get(ids[i]);
                if(pred(r)) local.add(&r);
            }
            std::lock_guard<std::mutex> lock(out_mtx);
            for(auto* ptr:local.ptrs) view.add(ptr);
        });
        return view;
    }
};

} // namespace mapdb
