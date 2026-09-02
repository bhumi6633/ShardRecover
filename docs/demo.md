# Demo

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

./build/shardrecover fragment legacy/ece252_starter/images/red-green-16x16.png \
  --size 64 --overlap 32 --shuffle --opaque-names --seed 1 \
  --corrupt-bytes 1 --output /tmp/shardrecover-fragments

./build/shardrecover reconstruct /tmp/shardrecover-fragments \
  --strategy beam --beam-width 8 --graph-build exhaustive \
  --min-overlap 32 --max-mismatches 1 --repair consensus \
  --output /tmp/shardrecover-recovered.png \
  --trace-json /tmp/shardrecover-trace.json

python3 -m json.tool /tmp/shardrecover-trace.json >/dev/null
```

Launch the workstation:

```sh
cd frontend
npm ci
npm run dev
```

Choose **Open Trace** and load `/tmp/shardrecover-trace.json`. Use seed `42` without `--corrupt-bytes` for a clean trace. Add `--format png` during clean reconstruction to populate PNG chunk and CRC evidence.
