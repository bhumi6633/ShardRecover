# Fuzzing ShardRecover

Fuzzing is opt-in and requires Clang with a libFuzzer runtime. Some Apple command-line-tools installations identify as AppleClang but omit that runtime; select a complete LLVM installation with `-DCMAKE_CXX_COMPILER=/path/to/clang++` when necessary.

Configure and build the fuzz targets:

```sh
cmake -S . -B build-fuzz \
  -DSHARDRECOVER_ENABLE_FUZZING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz
```

Run bounded sessions against the checked-in seed corpora:

```sh
./build-fuzz/shardrecover_png_fuzz fuzz/corpus/png -max_total_time=60
./build-fuzz/shardrecover_overlap_fuzz fuzz/corpus/overlap -max_total_time=60
```

Regenerate the small deterministic corpus with `python3 fuzz/generate_corpus.py`.

To run the ordinary tests with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-asan \
  -DSHARDRECOVER_ENABLE_SANITIZERS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Sanitizer and fuzzing options are disabled by default. A bounded local run is evidence only for that run; it does not prove the absence of defects.
