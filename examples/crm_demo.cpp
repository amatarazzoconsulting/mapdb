// MapDB CRM demo
// The quick-start program from README.md, built against the shipped header.
//
// Build:  make demo
// Run:    ./bin/crm_demo
//
// Delete the *.db files in the working directory to start from scratch.

#include "mapdb.hpp"

#include <iostream>
#include <string>
#include <chrono>

struct Customer {
    int id;
    mapdb::Name name;
    mapdb::FixedString<96> email;
    std::chrono::system_clock::time_point registration_date;
};

struct Interaction {
    int id;
    int customerId;
    mapdb::FixedString<32> type;  // "email", "call", "meeting"
    mapdb::FixedString<128> notes;
    std::chrono::system_clock::time_point timestamp;
};

struct Sale {
    int saleId;
    int customerId;
    double amount;
    std::chrono::system_clock::time_point timestamp;
};

int main() {
    using namespace mapdb;

    Storage<Customer> customers("customers.db");
    Storage<Interaction> interactions("interactions.db");
    Storage<Sale> sales("sales.db");

    // Indexes. Adding an index also backfills it from records already on disk,
    // so this is safe to call every run.
    customers.addHashIndex(&Customer::id);
    customers.addTrieIndex(&Customer::name);

    interactions.addHashIndex(&Interaction::customerId);

    sales.addHashIndex(&Sale::customerId);
    sales.addHashIndex(&Sale::saleId);
    sales.addTrieIndex(&Sale::amount);  // enables prefix() on the printed amount

    // Seed the tables only once, so repeated runs show persistence instead of
    // duplicating rows.
    if (customers.empty()) {
        auto now = std::chrono::system_clock::now();

        customers.insert({1, "Alice Smith", "alice@example.com", now});
        customers.insert({2, "Bob Johnson", "bob@example.com", now - std::chrono::hours(24 * 30)});
        customers.insert({3, "Carol White", "carol@example.com", now - std::chrono::hours(24 * 90)});

        interactions.insert({1, 1, "email", "Discussed product features", now});
        interactions.insert({2, 2, "call", "Follow-up on invoice", now - std::chrono::hours(12)});
        interactions.insert({3, 1, "meeting", "Demo presentation", now - std::chrono::hours(48)});

        sales.insert({101, 1, 250.50, now});
        sales.insert({102, 2, 120.00, now - std::chrono::hours(24)});
        sales.insert({103, 1, 75.75, now - std::chrono::hours(72)});
        sales.insert({104, 3, 310.00, now - std::chrono::hours(5)});
    }

    std::cout << "Tables: " << customers.size() << " customers, " << interactions.size()
              << " interactions, " << sales.size() << " sales\n";

    // --- exact lookup through the hash index -------------------------------
    std::cout << "\nCustomer 2: ";
    for (auto* c : customers.find(2)) {
        std::cout << c->id << " " << c->name.c_str() << " <" << c->email.c_str() << ">";
    }
    std::cout << "\n";

    // --- prefix search through the trie index ------------------------------
    std::cout << "\nCustomers starting with 'Al':\n";
    for (auto* c : customers.prefix("Al")) {
        std::cout << "  " << c->id << " " << c->name.c_str() << "\n";
    }

    // --- pointer view with a predicate -------------------------------------
    std::cout << "\nCustomers whose name starts with 'A':\n";
    auto viewA = customers.view([](const Customer& c) { return c.name.starts_with("A"); });
    for (auto* c : viewA) {
        std::cout << "  " << c->id << " " << c->name.c_str() << " " << c->email.c_str() << "\n";
    }

    // --- time based query --------------------------------------------------
    auto now = std::chrono::system_clock::now();
    std::cout << "\nInteractions in the last 48 hours:\n";
    auto recent = interactions.view([now](const Interaction& i) {
        return i.timestamp > now - std::chrono::hours(48);
    });
    for (auto* i : recent) {
        std::cout << "  CustomerID: " << i->customerId << ", Type: " << i->type.c_str()
                  << ", Notes: " << i->notes.c_str() << "\n";
    }

    // --- join --------------------------------------------------------------
    std::cout << "\nCustomers with sales over $100:\n";
    auto highValue = mapdb::join(customers, sales, &Customer::id, &Sale::customerId,
                                 [](const Customer*, const Sale* s) { return s->amount > 100.0; });
    for (auto& pair : highValue) {
        std::cout << "  " << pair.first->name.c_str() << " purchased $" << pair.second->amount << "\n";
    }

    // --- parallel query ----------------------------------------------------
    std::cout << "\nParallel query for sales over $100:\n";
    auto big = sales.parallelQuery([](const Sale& s) { return s.amount > 100; });
    for (auto* s : big) {
        std::cout << "  CustomerID: " << s->customerId << ", Amount: $" << s->amount << "\n";
    }

    // --- total per customer ------------------------------------------------
    std::cout << "\nTotal sales per customer:\n";
    for (auto* c : customers.all()) {
        double total = 0;
        for (auto* s : sales.view([c](const Sale& s) { return s.customerId == c->id; })) total += s->amount;
        std::cout << "  " << c->name.c_str() << ": $" << total << "\n";
    }

    // --- persistence -------------------------------------------------------
    customers.flush();
    interactions.flush();
    sales.flush();
    std::cout << "\nFlushed to disk. Rerun this program: the rows are still here.\n";
    return 0;
}
