// MapDB advanced demo: multi-table joins, wildcards and pointer views.
// This is the program shown in the "Set Traversal" section of README.md.
//
// Build:  make demo
// Run:    ./bin/advanced_demo
//
// Records are trivially copyable, so text fields use mapdb::FixedString and
// timestamps use std::chrono::system_clock::time_point.

#include "mapdb.hpp"

#include <chrono>
#include <iostream>
#include <string>

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

struct Product {
    int productId;
    mapdb::Name name;
    mapdb::FixedString<32> category;
    double price;
};

struct Order {
    int orderId;
    int customerId;
    int productId;
    int quantity;
    std::chrono::system_clock::time_point timestamp;
};

int main() {
    using namespace mapdb;

    Storage<Customer> customers("customers.db");
    Storage<Sale> sales("sales.db");
    Storage<Product> products("products.db");
    Storage<Order> orders("orders.db");

    customers.addHashIndex(&Customer::id);
    customers.addTrieIndex(&Customer::name);

    sales.addHashIndex(&Sale::customerId);
    sales.addTrieIndex(&Sale::amount);

    products.addHashIndex(&Product::productId);
    products.addTrieIndex(&Product::category);

    orders.addHashIndex(&Order::customerId);
    orders.addHashIndex(&Order::productId);

    if (products.empty()) {
        auto now = std::chrono::system_clock::now();

        customers.insert({1, "Alice Smith", "alice@example.com", now - std::chrono::hours(24 * 100)});
        customers.insert({2, "Bob Johnson", "bob@example.com", now - std::chrono::hours(24 * 50)});
        customers.insert({3, "Carol White", "carol@example.com", now - std::chrono::hours(24 * 30)});

        products.insert({101, "Blue Shirt", "Clothing", 29.99});
        products.insert({102, "Red Shirt", "Clothing", 35.99});
        products.insert({103, "Gaming Mouse", "Electronics", 59.99});

        sales.insert({201, 1, 250.50, now - std::chrono::hours(10)});
        sales.insert({202, 2, 120.00, now - std::chrono::hours(5)});
        sales.insert({203, 1, 75.75, now - std::chrono::hours(50)});
        sales.insert({204, 3, 300.00, now - std::chrono::hours(2)});

        orders.insert({301, 1, 101, 2, now - std::chrono::hours(10)});
        orders.insert({302, 2, 102, 1, now - std::chrono::hours(5)});
        orders.insert({303, 1, 103, 1, now - std::chrono::hours(50)});
        orders.insert({304, 3, 103, 2, now - std::chrono::hours(2)});
    }

    std::cout << "=== Customers Starting with 'A' ===\n";
    for (auto* c : customers.view([](const Customer& x) { return x.name.starts_with("A"); })) {
        std::cout << c->id << " " << c->name.c_str() << "\n";
    }

    std::cout << "\n=== Sales Over $200 ===\n";
    for (auto* s : sales.view([](const Sale& x) { return x.amount > 200; })) {
        std::cout << "CustomerID: " << s->customerId << " Amount: $" << s->amount << "\n";
    }

    std::cout << "\n=== Join Customers and Sales Over $100 ===\n";
    auto customerSales = mapdb::join(customers, sales, &Customer::id, &Sale::customerId,
                                     [](const Customer*, const Sale* s) { return s->amount > 100; });
    for (auto& pair : customerSales) {
        std::cout << pair.first->name.c_str() << " purchased $" << pair.second->amount << "\n";
    }

    std::cout << "\n=== Join Orders with Products and Customers ===\n";
    auto orderDetails = mapdb::join(orders, products, &Order::productId, &Product::productId);
    for (auto& pair : orderDetails) {
        const Order* o = pair.first;
        const Product* p = pair.second;
        auto who = customers.find(o->customerId);
        std::cout << "Customer: " << (who.empty() ? "?" : who[0]->name.c_str()) << ", Product: "
                  << p->name.c_str() << ", Qty: " << o->quantity << "\n";
    }

    std::cout << "\n=== Partial Key Search: Products with 'Shirt' ===\n";
    for (auto* p : products.view([](const Product& x) {
             return mapdb::wildcard_match("*Shirt*", x.name.sv());
         })) {
        std::cout << p->name.c_str() << " $" << p->price << "\n";
    }

    std::cout << "\n=== Complex Query: Orders of more than one Electronics item ===\n";
    auto electronics = mapdb::join(orders, products, &Order::productId, &Product::productId,
                                   [](const Order* o, const Product* p) {
                                       return p->category == std::string_view("Electronics") && o->quantity > 1;
                                   });
    for (auto& pair : electronics) {
        const Order* o = pair.first;
        const Product* p = pair.second;
        auto who = customers.find(o->customerId);
        std::cout << (who.empty() ? "?" : who[0]->name.c_str()) << " ordered " << o->quantity << " x "
                  << p->name.c_str() << "\n";
    }

    std::cout << "\n=== Iteration of All Customers and Their Total Sales ===\n";
    for (auto* c : customers.all()) {
        double total = 0;
        for (auto* s : sales.view([c](const Sale& x) { return x.customerId == c->id; })) total += s->amount;
        std::cout << c->name.c_str() << " total sales: $" << total << "\n";
    }

    customers.flush();
    sales.flush();
    products.flush();
    orders.flush();
    return 0;
}
