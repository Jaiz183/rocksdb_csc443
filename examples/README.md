1. Compile RocksDB first by executing `make static_lib` in parent dir
2. Compile all examples: `cd examples/; make all`

Run the XOR and Bloom filter query comparison with separate database paths.
The first path is a prefix; the example appends each XOR fingerprint width:
`./xor_filter_query_example /tmp/rocksdb_xor_filter /tmp/rocksdb_bloom`

The example compares 1-, 2-, 3-, 4-, and 8-byte XOR fingerprints by
default. Pass one or more integer sizes from 1 to 8 bytes to select different
sizes. For every filter, it writes and flushes 10,000 keys, warms the present-
and absent-key query sets once, then reports flush time, present-key query
throughput, and the measured false-positive rate from RocksDB's full-filter
counters over 1,000,000 absent queries. Use fresh database paths for each run.
Results are indicative and depend on the machine and cache state.
