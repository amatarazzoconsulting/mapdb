# Scientific dataset benchmark

`scientific_benchmark` generates deterministic synthetic station observations (timestamp, station ID, coordinates, measurement, and quality flag), stores them using `mapdb.hpp`, then measures insertion, indexed station lookups, and a parallel filter scan.

The MapDB timings are measured on the machine where the program runs. **The MySQL values are not measured and the program does not connect to MySQL**: they are configurable model estimates included to demonstrate how assumptions change the comparison. Replace the illustrative defaults with throughput/latency values measured on your own MySQL deployment before drawing conclusions. A fair real-world comparison should use the same hardware, data, indexes, durability settings, cache state, and query workload for both engines.

Build and run:

```sh
make scientific-benchmark
./bin/scientific_benchmark --rows 1000000
```

Adjust the MySQL model assumptions or dataset size:

```sh
./bin/scientific_benchmark --rows 5000000 \
  --mysql-insert-rows-sec 50000 \
  --mysql-scan-rows-sec 2000000 \
  --mysql-query-ms 1.5
```

The output labels MapDB as measured and MySQL as a model. `model/MapDB` is a ratio of those unlike measurement types, so it is only illustrative unless the MySQL parameters come from a comparable measured run. Large row counts require disk space for the mapped file and RAM for MapDB's in-memory hash index.
