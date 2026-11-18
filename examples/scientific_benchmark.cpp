#include "mapdb.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

struct ScientificSample {
    int64_t timestamp_seconds;
    mapdb::FixedString<32> station;
    double latitude;
    double longitude;
    double measurement;
    int32_t quality_flag;
};

struct Options {
    size_t rows = 1'000'000;
    double mysql_insert_rows_per_second = 25'000.0;
    double mysql_scan_rows_per_second = 1'000'000.0;
    double mysql_query_milliseconds = 2.0;
};

size_t parse_size(const std::string& value, const char* name) {
    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno || end == value.c_str() || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>(SIZE_MAX)) {
        throw std::invalid_argument(std::string("invalid value for ") + name + ": " + value);
    }
    return static_cast<size_t>(parsed);
}

double parse_positive_double(const std::string& value, const char* name) {
    char* end = nullptr;
    errno = 0;
    double parsed = std::strtod(value.c_str(), &end);
    if (errno || end == value.c_str() || *end != '\0' || !std::isfinite(parsed) || parsed <= 0.0) {
        throw std::invalid_argument(std::string("invalid value for ") + name + ": " + value);
    }
    return parsed;
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: scientific_benchmark [--rows N] "
                         "[--mysql-insert-rows-sec N] [--mysql-scan-rows-sec N] "
                         "[--mysql-query-ms N]\n"
                         "Runs MapDB against a clearly labeled, configurable MySQL cost model; "
                         "it does not connect to MySQL.\n";
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::invalid_argument("missing value after " + arg);
        const std::string value = argv[++i];
        if (arg == "--rows") options.rows = parse_size(value, "--rows");
        else if (arg == "--mysql-insert-rows-sec")
            options.mysql_insert_rows_per_second = parse_positive_double(value, "--mysql-insert-rows-sec");
        else if (arg == "--mysql-scan-rows-sec")
            options.mysql_scan_rows_per_second = parse_positive_double(value, "--mysql-scan-rows-sec");
        else if (arg == "--mysql-query-ms")
            options.mysql_query_milliseconds = parse_positive_double(value, "--mysql-query-ms");
        else throw std::invalid_argument("unknown option: " + arg);
    }
    return options;
}

ScientificSample make_sample(size_t i) {
    ScientificSample sample{};
    sample.timestamp_seconds = 1'700'000'000 + static_cast<int64_t>(i);
    sample.station = "station-" + std::to_string(i % 1000);
    sample.latitude = -90.0 + static_cast<double>((i * 37) % 18000) / 100.0;
    sample.longitude = -180.0 + static_cast<double>((i * 71) % 36000) / 100.0;
    sample.measurement = 250.0 + static_cast<double>((i * 13) % 5000) / 100.0;
    sample.quality_flag = static_cast<int32_t>(i % 5);
    return sample;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void print_result(const char* operation, double mapdb_seconds, double mysql_model_seconds) {
    std::cout << std::left << std::setw(24) << operation << std::right
              << " MapDB measured: " << std::setw(10) << mapdb_seconds << " s"
              << " | MySQL model: " << std::setw(10) << mysql_model_seconds << " s"
              << " | model/MapDB: " << std::fixed << std::setprecision(2)
              << (mapdb_seconds > 0.0 ? mysql_model_seconds / mapdb_seconds : 0.0) << "x\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_args(argc, argv);
        const std::string path = "/tmp/mapdb_science_benchmark_" + std::to_string(::getpid()) + ".db";
        (void)::unlink(path.c_str());

        const size_t estimated_slot_bytes = sizeof(ScientificSample) + sizeof(uint64_t) + alignof(ScientificSample);
        if (options.rows > (SIZE_MAX - (size_t(1) << 20)) / estimated_slot_bytes)
            throw std::overflow_error("requested row count is too large");
        const size_t capacity = options.rows * estimated_slot_bytes + (size_t(1) << 20);

        double insert_seconds = 0.0;
        double exact_lookup_seconds = 0.0;
        double scan_seconds = 0.0;
        size_t matched = 0;
        size_t lookup_hits = 0;
        constexpr size_t lookup_count = 100;

        {
            mapdb::Storage<ScientificSample> samples(path, capacity);
            samples.addHashIndex(&ScientificSample::station);

            auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < options.rows; ++i) samples.insert(make_sample(i));
            insert_seconds = seconds_since(start);
            samples.flush();

            start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < lookup_count; ++i) {
                mapdb::FixedString<32> station("station-" + std::to_string((i * 17) % 1000));
                lookup_hits += samples.find(station).size();
            }
            exact_lookup_seconds = seconds_since(start);

            start = std::chrono::steady_clock::now();
            auto selected = samples.parallelView([](const ScientificSample& sample) {
                return sample.quality_flag == 0 && sample.measurement > 275.0;
            });
            matched = selected.size();
            scan_seconds = seconds_since(start);
        }
        (void)::unlink(path.c_str());

        const double mysql_insert_seconds = static_cast<double>(options.rows) / options.mysql_insert_rows_per_second;
        const double mysql_lookup_seconds = lookup_count * options.mysql_query_milliseconds / 1000.0;
        const double mysql_scan_seconds = static_cast<double>(options.rows) / options.mysql_scan_rows_per_second;
        const double mapdb_insert_rate = insert_seconds > 0.0 ? options.rows / insert_seconds : 0.0;
        const double mapdb_scan_rate = scan_seconds > 0.0 ? options.rows / scan_seconds : 0.0;

        std::cout << "Scientific dataset benchmark (synthetic deterministic records)\n"
                  << "Rows: " << options.rows << " | MapDB record size: " << sizeof(ScientificSample)
                  << " bytes | matching scan rows: " << matched << " | exact lookup hits: " << lookup_hits << "\n"
                  << "MySQL figures below are MODEL ESTIMATES, not measured MySQL results. "
                     "Tune the --mysql-* assumptions for your environment.\n\n"
                  << std::fixed << std::setprecision(4);
        print_result("Bulk insert", insert_seconds, mysql_insert_seconds);
        print_result("100 indexed lookups", exact_lookup_seconds, mysql_lookup_seconds);
        print_result("Parallel filter scan", scan_seconds, mysql_scan_seconds);
        std::cout << "Measured MapDB rates: " << std::setprecision(0) << mapdb_insert_rate
                  << " inserts/s, " << mapdb_scan_rate << " scanned rows/s\n";
        std::cout << "MySQL model assumptions: " << options.mysql_insert_rows_per_second
                  << " insert rows/s, " << options.mysql_scan_rows_per_second
                  << " scan rows/s, " << options.mysql_query_milliseconds << " ms/query\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "scientific_benchmark: " << e.what() << "\n";
        return 1;
    }
}
