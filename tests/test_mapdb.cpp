// MapDB test suite
// Build and run with:  make test
//
// Every test uses its own scratch file under ./tmp so runs are independent and
// the persistence test can reopen a file written by an earlier phase.

#include "mapdb.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
std::string g_current_test;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(cond)) {                                                                     \
            ++g_failures;                                                                  \
            std::cout << "  FAIL [" << g_current_test << "] " << __FILE__ << ":" << __LINE__ \
                      << "  " #cond "\n";                                                  \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        ++g_checks;                                                                        \
        auto&& va = (a);                                                                   \
        auto&& vb = (b);                                                                   \
        if (!(va == vb)) {                                                                 \
            ++g_failures;                                                                  \
            std::cout << "  FAIL [" << g_current_test << "] " << __FILE__ << ":" << __LINE__ \
                      << "  " #a " == " #b " (" << va << " vs " << vb << ")\n";            \
        }                                                                                  \
    } while (0)

#define CHECK_THROWS(expr, extype)                                                         \
    do {                                                                                   \
        ++g_checks;                                                                        \
        bool threw = false;                                                                \
        try {                                                                              \
            (void)(expr);                                                                  \
        } catch (const extype&) {                                                          \
            threw = true;                                                                  \
        } catch (...) {                                                                    \
        }                                                                                  \
        if (!threw) {                                                                      \
            ++g_failures;                                                                  \
            std::cout << "  FAIL [" << g_current_test << "] " << __FILE__ << ":" << __LINE__ \
                      << "  expected " #extype " from " #expr "\n";                        \
        }                                                                                  \
    } while (0)

void begin(const char* name) {
    g_current_test = name;
    std::cout << "- " << name << "\n";
}

// A record shaped like the CRM examples in the README: trivially copyable,
// text stored in fixed buffers, time stored as a time_point.
struct Customer {
    int id;
    mapdb::Name name;
    mapdb::FixedString<96> email;
    std::chrono::system_clock::time_point registration_date;
};

struct Sale {
    int saleId;
    int customerId;
    double amount;
    std::chrono::system_clock::time_point timestamp;
};

struct Interaction {
    int id;
    int customerId;
    mapdb::FixedString<32> type;
    std::chrono::system_clock::time_point timestamp;
};

std::string scratch(const std::string& leaf) {
    std::string dir = "tmp";
    ::system(("mkdir -p " + dir).c_str());
    return dir + "/" + leaf;
}

void reset(const std::string& path) { std::remove(path.c_str()); }

/* ---------------------------------------------------------------- */

void test_fixed_string() {
    begin("FixedString");

    static_assert(std::is_trivially_copyable<mapdb::FixedString<8>>::value,
                  "FixedString must be trivially copyable to live in the mapping");
    static_assert(std::is_trivially_copyable<Customer>::value, "Customer must be trivially copyable");

    mapdb::FixedString<8> s("Alice");
    CHECK_EQ(std::string(s.c_str()), std::string("Alice"));
    CHECK_EQ(s.size(), size_t(5));
    CHECK(s.starts_with("Al"));
    CHECK(!s.starts_with("Bo"));
    CHECK(s.contains("lic"));
    CHECK(!s.contains("zzz"));

    s = "0123456789";  // longer than the buffer: truncated to 7 chars + NUL
    CHECK_EQ(std::string(s.c_str()), std::string("0123456"));
    CHECK_EQ(s.size(), size_t(7));

    mapdb::FixedString<8> empty;
    CHECK(empty.empty());
    CHECK_EQ(std::string(empty.c_str()), std::string(""));

    mapdb::FixedString<8> a("Bob"), b("Bob"), c("Carol");
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a < c);
}

void test_codecs() {
    begin("Compression codecs");

    // Nibble: even length
    std::vector<uint8_t> values{1, 2, 3, 4, 15, 0};
    std::vector<uint8_t> packed, restored;
    mapdb::NibbleCompressor::compress(values, packed);
    CHECK_EQ(packed.size(), size_t(3));
    mapdb::NibbleCompressor::decompress(packed, restored, values.size());
    CHECK(restored == values);

    // Nibble: odd length must round trip exactly
    std::vector<uint8_t> odd{7, 3, 9};
    mapdb::NibbleCompressor::compress(odd, packed);
    mapdb::NibbleCompressor::decompress(packed, restored, odd.size());
    CHECK(restored == odd);

    // RLE
    using ByteRLE = mapdb::RLE<uint8_t>;
    std::vector<uint8_t> runs{5, 5, 5, 1, 2, 2, 9, 9, 9, 9};
    auto encoded = ByteRLE::compress(runs);
    CHECK_EQ(encoded.size(), size_t(4));  // (5,3) (1,1) (2,2) (9,4)
    CHECK_EQ(encoded[0].count, 3u);
    CHECK(ByteRLE::decompress(encoded) == runs);

    // A run longer than 255 must not be truncated by a narrow value type
    std::vector<uint8_t> uniform(1000, 42);
    auto uniform_encoded = ByteRLE::compress(uniform);
    CHECK_EQ(uniform_encoded.size(), size_t(1));
    CHECK_EQ(uniform_encoded[0].count, 1000u);
    CHECK(ByteRLE::decompress(uniform_encoded) == uniform);

    std::vector<int> empty_rle;
    using IntRLE = mapdb::RLE<int>;
    CHECK(IntRLE::compress(empty_rle).empty());
    CHECK(IntRLE::decompress(IntRLE::compress(empty_rle)).empty());

    // Delta
    std::vector<int64_t> series{100, 103, 106, 100, 1};
    CHECK(mapdb::Delta<int64_t>::decode(mapdb::Delta<int64_t>::encode(series)) == series);

    std::vector<uint32_t> uns{0, 5, 4, 4000000000u, 1};
    CHECK(mapdb::Delta<uint32_t>::decode(mapdb::Delta<uint32_t>::encode(uns)) == uns);

    std::vector<int64_t> empty_delta;
    CHECK(mapdb::Delta<int64_t>::encode(empty_delta).empty());
    CHECK(mapdb::Delta<int64_t>::decode(empty_delta).empty());

    // Corrupt stream is rejected rather than silently truncated
    std::vector<ByteRLE::Run> bad{{1, 0}, {2, 3}};
    CHECK_THROWS(ByteRLE::decompress(bad), std::runtime_error);
}

void test_time_utils() {
    begin("Time parsing and formatting");

    auto t = mapdb::parse_time("2026-01-31T12:34:56");
    CHECK_EQ(mapdb::format_time(t), std::string("2026-01-31T12:34:56"));

    auto date_only = mapdb::parse_time("2026-01-31", "%Y-%m-%d");
    CHECK_EQ(mapdb::format_time(date_only, "%Y-%m-%d"), std::string("2026-01-31"));
    CHECK_EQ(mapdb::format_time(date_only), std::string("2026-01-31T00:00:00"));

    CHECK_THROWS(mapdb::parse_time("not a date"), std::runtime_error);

    // A time_point is a usable key: hashing and ordering both behave
    mapdb::Hash<std::chrono::system_clock::time_point> h;
    auto later = t + std::chrono::hours(1);
    CHECK(h(t) != h(later));
    CHECK(t < later);
    CHECK_EQ(mapdb::to_key_string(t), std::string("2026-01-31T12:34:56"));
}

void test_wildcard() {
    begin("Wildcard matching");

    CHECK(mapdb::wildcard_match("Jo*", "Johnson"));
    CHECK(mapdb::wildcard_match("*son", "Johnson"));
    CHECK(mapdb::wildcard_match("J?hn*", "Johnson"));
    CHECK(mapdb::wildcard_match("*", ""));
    CHECK(mapdb::wildcard_match("", ""));
    CHECK(mapdb::wildcard_match("Alice", "Alice"));
    CHECK(!mapdb::wildcard_match("Alice", "Alicia"));
    CHECK(!mapdb::wildcard_match("", "x"));
    CHECK(mapdb::wildcard_match("a*b*c", "axxbyyc"));
    CHECK(!mapdb::wildcard_match("a*b*c", "axxbyy"));
}

void test_trie() {
    begin("Radix trie");

    mapdb::RadixTrie trie;
    trie.insert("Alice", 10);
    trie.insert("Alfred", 11);
    trie.insert("Bob", 12);

    auto al = trie.prefix("Al");
    CHECK_EQ(al.size(), size_t(2));
    CHECK((std::set<size_t>(al.begin(), al.end()) == std::set<size_t>{10, 11}));

    CHECK_EQ(trie.prefix("Ali").size(), size_t(1));
    CHECK(trie.prefix("Z").empty());
    CHECK_EQ(trie.prefix("").size(), size_t(3));  // empty prefix matches everything
}

void test_storage_basics() {
    begin("Storage insert / find / view / prefix");

    const std::string path = scratch("basics.db");
    reset(path);

    auto now = mapdb::parse_time("2026-05-11T00:00:00");
    {
        mapdb::Storage<Customer> customers(path);
        customers.addHashIndex(&Customer::id);
        customers.addTrieIndex(&Customer::name);

        customers.insert({1, "Alice Smith", "alice@example.com", now});
        customers.insert({2, "Alfred Jones", "alfred@example.com", now - std::chrono::hours(24 * 30)});
        customers.insert({3, "Bob Johnson", "bob@example.com", now - std::chrono::hours(24 * 90)});
        customers.insert({1, "Alice Clone", "clone@example.com", now});  // duplicate key on purpose

        CHECK_EQ(customers.size(), size_t(4));
        CHECK(!customers.empty());

        // exact lookup through the hash index
        auto byId = customers.find(1);
        CHECK_EQ(byId.size(), size_t(2));
        for (auto* c : byId) CHECK_EQ(c->id, 1);

        auto missing = customers.find(99);
        CHECK(missing.empty());

        // prefix lookup through the trie
        auto prefixAl = customers.prefix("Al");
        CHECK_EQ(prefixAl.size(), size_t(3));  // Alice Smith, Alfred Jones, Alice Clone

        // filtered views, pointer semantics
        auto startingWithA = customers.view([](const Customer& c) { return c.name.starts_with("A"); });
        CHECK_EQ(startingWithA.size(), size_t(3));
        int seen = 0;
        for (auto* c : startingWithA) {
            CHECK(c->name.starts_with("A"));
            ++seen;
        }
        CHECK_EQ(seen, 3);
        CHECK(startingWithA[0] != nullptr);

        // parallel view returns the same records as the serial one
        auto parallel = customers.parallelView([](const Customer& c) { return c.name.starts_with("A"); });
        CHECK_EQ(parallel.size(), startingWithA.size());
        auto viaAlias = customers.parallelQuery([](const Customer& c) { return c.id == 3; });
        CHECK_EQ(viaAlias.size(), size_t(1));

        // where() over explicit ids
        std::vector<size_t> ids;
        for (size_t id = 0; id < customers.size(); ++id) ids.push_back(id * mapdb::RecordStore<Customer>::slot_size());
        auto filtered = customers.where(ids, [](const Customer& c) { return c.id == 2; });
        CHECK_EQ(filtered.size(), size_t(1));

        // time stored in the record survives the memcpy intact
        auto all = customers.all();
        CHECK_EQ(all.size(), size_t(4));
        CHECK(all[0]->registration_date == now);

        CHECK_THROWS(customers.find(std::string("no such index type")), std::runtime_error);
        CHECK_THROWS(customers.at(999999), std::out_of_range);

        customers.flush();
    }
    reset(path);
}

void test_persistence() {
    begin("Persistence across reopen");

    const std::string path = scratch("persist.db");
    reset(path);

    auto t0 = mapdb::parse_time("2026-02-01T08:00:00");
    {
        mapdb::Storage<Customer> customers(path);
        customers.addHashIndex(&Customer::id);
        customers.addTrieIndex(&Customer::name);
        customers.insert({1, "Alice Smith", "alice@example.com", t0});
        customers.insert({2, "Bob Johnson", "bob@example.com", t0 + std::chrono::hours(1)});
        customers.flush();
    }

    {
        // Reopen: records must still be there, and indexes must be rebuilt.
        mapdb::Storage<Customer> customers(path);
        CHECK_EQ(customers.size(), size_t(2));
        customers.addHashIndex(&Customer::id);
        customers.addTrieIndex(&Customer::name);

        CHECK_EQ(customers.find(2).size(), size_t(1));
        CHECK_EQ(customers.prefix("Al").size(), size_t(1));

        auto* alice = customers.find(1)[0];
        CHECK_EQ(std::string(alice->name.c_str()), std::string("Alice Smith"));
        CHECK_EQ(std::string(alice->email.c_str()), std::string("alice@example.com"));
        CHECK(alice->registration_date == t0);

        // Appending after reopen must not overwrite the existing slots.
        customers.insert({3, "Carol White", "carol@example.com", t0 + std::chrono::hours(2)});
        customers.flush();
        CHECK_EQ(customers.size(), size_t(3));
    }

    {
        mapdb::Storage<Customer> customers(path);
        CHECK_EQ(customers.size(), size_t(3));
        auto all = customers.all();
        CHECK_EQ(std::string(all[0]->name.c_str()), std::string("Alice Smith"));
        CHECK_EQ(std::string(all[2]->name.c_str()), std::string("Carol White"));
    }

    reset(path);
}

void test_alignment() {
    begin("Record alignment");

    struct alignas(16) Wide {
        double a;
        double b;
        int id;
    };
    static_assert(alignof(Wide) == 16, "test assumes 16-byte alignment");

    const std::string path = scratch("align.db");
    reset(path);
    {
        mapdb::Storage<Wide> table(path);
        for (int i = 0; i < 50; ++i) table.insert({i * 1.5, i * 2.5, i});
        for (auto* w : table.all()) {
            CHECK(reinterpret_cast<uintptr_t>(w) % alignof(Wide) == 0);
        }
        CHECK_EQ(table.all()[49]->id, 49);
    }
    reset(path);
}

void test_capacity_limit() {
    begin("Capacity limit");

    const std::string path = scratch("tiny.db");
    reset(path);

    const size_t slot = mapdb::RecordStore<Customer>::slot_size();
    mapdb::Storage<Customer> customers(path, slot * 2);  // room for exactly two records
    auto t0 = mapdb::parse_time("2026-03-01T00:00:00");
    customers.insert({1, "A", "a@x", t0});
    customers.insert({2, "B", "b@x", t0});
    CHECK_EQ(customers.size(), size_t(2));
    CHECK_THROWS(customers.insert({3, "C", "c@x", t0}), std::runtime_error);

    // Growing the mapping later is supported and keeps existing records.
    customers.flush();
    mapdb::Storage<Customer> grown(path, slot * 8);
    CHECK_EQ(grown.size(), size_t(2));
    grown.insert({3, "C", "c@x", t0});
    CHECK_EQ(grown.size(), size_t(3));
    reset(path);
}

void test_join() {
    begin("Join engine");

    const std::string cp = scratch("join_customers.db");
    const std::string sp = scratch("join_sales.db");
    reset(cp);
    reset(sp);

    auto now = mapdb::parse_time("2026-04-01T00:00:00");
    mapdb::Storage<Customer> customers(cp);
    mapdb::Storage<Sale> sales(sp);

    customers.insert({1, "Alice Smith", "alice@example.com", now});
    customers.insert({2, "Bob Johnson", "bob@example.com", now});
    customers.insert({3, "Carol White", "carol@example.com", now});

    sales.insert({101, 1, 250.50, now});
    sales.insert({102, 2, 120.00, now});
    sales.insert({103, 1, 75.75, now});
    sales.insert({104, 9, 999.00, now});  // orphan sale, no matching customer

    auto pairs = mapdb::join(customers, sales, &Customer::id, &Sale::customerId);
    CHECK_EQ(pairs.size(), size_t(3));  // orphan excluded

    auto big = mapdb::join(customers, sales, &Customer::id, &Sale::customerId,
                           [](const Customer*, const Sale* s) { return s->amount > 100.0; });
    CHECK_EQ(big.size(), size_t(2));
    for (auto& p : big) CHECK(p.second->amount > 100.0);

    double totalForAlice = 0;
    for (auto& p : pairs) {
        if (p.first->id == 1) totalForAlice += p.second->amount;
    }
    CHECK(totalForAlice > 326.2 && totalForAlice < 326.3);

    reset(cp);
    reset(sp);
}

void test_query_ast() {
    begin("QueryAST");

    const std::string path = scratch("ast.db");
    reset(path);
    mapdb::Storage<Sale> sales(path);
    auto now = mapdb::parse_time("2026-06-01T00:00:00");
    sales.insert({1, 1, 10.0, now});
    sales.insert({2, 1, 500.0, now});
    sales.insert({3, 2, 800.0, now});

    mapdb::QueryAST<Sale> ast;
    ast.where([](const Sale& s) { return s.amount > 100.0; })
        .where([](const Sale& s) { return s.customerId == 1; });

    auto result = mapdb::query(sales, ast);
    CHECK_EQ(result.size(), size_t(1));
    CHECK_EQ(result[0]->saleId, 2);

    mapdb::QueryAST<Sale> empty_ast;
    CHECK_EQ(mapdb::query(sales, empty_ast).size(), size_t(3));
    reset(path);
}

void test_parallel_correctness() {
    begin("Parallel view on a large table");

    const std::string path = scratch("parallel.db");
    reset(path);

    const int n = 200000;
    mapdb::Storage<Interaction> table(path, size_t(n) * mapdb::RecordStore<Interaction>::slot_size() + 4096);
    auto t0 = mapdb::parse_time("2026-01-01T00:00:00");
    for (int i = 0; i < n; ++i) {
        table.insert({i, i % 97, i % 3 == 0 ? "email" : "call", t0 + std::chrono::seconds(i)});
    }
    CHECK_EQ(table.size(), size_t(n));

    auto serial = table.view([](const Interaction& x) { return x.customerId == 5; });
    auto parallel = table.parallelView([](const Interaction& x) { return x.customerId == 5; });

    std::set<size_t> serial_ids, parallel_ids;
    for (auto* x : serial) serial_ids.insert(static_cast<size_t>(x->id));
    for (auto* x : parallel) parallel_ids.insert(static_cast<size_t>(x->id));

    CHECK_EQ(serial_ids.size(), parallel_ids.size());
    CHECK(serial_ids == parallel_ids);
    size_t expected = 0;  // count ids i in [0,n) with i % 97 == 5
    for (int i = 5; i < n; i += 97) ++expected;
    CHECK_EQ(serial_ids.size(), expected);

    // time range query on stored time_points
    auto first_hour = table.view([t0](const Interaction& x) {
        return x.timestamp < t0 + std::chrono::hours(1);
    });
    CHECK_EQ(first_hour.size(), size_t(3600));

    reset(path);
}

void test_move_and_errors() {
    begin("Error handling");

    mapdb::Storage<Customer> table(scratch("errors.db"));
    CHECK_THROWS(table.find(1), std::runtime_error);  // no hash index registered yet
    CHECK_EQ(table.size(), size_t(0));
    CHECK(table.empty());
    reset(scratch("errors.db"));

    CHECK_THROWS(mapdb::MemoryMappedFile().open("/proc/definitely/not/writable.db", 4096),
                 std::runtime_error);
}

}  // namespace

int main() {
    std::cout << "MapDB test suite\n";
    test_fixed_string();
    test_codecs();
    test_time_utils();
    test_wildcard();
    test_trie();
    test_storage_basics();
    test_persistence();
    test_alignment();
    test_capacity_limit();
    test_join();
    test_query_ast();
    test_parallel_correctness();
    test_move_and_errors();

    std::cout << "\n" << g_checks - g_failures << "/" << g_checks << " checks passed\n";
    if (g_failures) {
        std::cout << g_failures << " FAILURE(S)\n";
        return 1;
    }
    std::cout << "All tests passed\n";
    return 0;
}
