# Reconstruction trace schema

ShardRecover emits schema version `1` with `--trace-json <file>`. The root contains `engine`, `configuration`, `fragments`, `edges`, `selected_path`, `reconstruction`, `mismatches`, `repairs`, `format`, and `graph_stats`.

Fragments contain stable graph-node `id`, display `name`, and byte `size`. Edges contain observed overlap length, match/mismatch counts, exactness, and mismatch observations. Individual observed bytes are JSON integers from 0 through 255. `selected_path` is the ordered node route; selected mismatches identify its adjacent node IDs and overlap offsets.

Repairs report the actual `consensus` or `png_crc` stage evidence. PNG format evidence includes validation flags, dimensions, CRC counts, and compact chunk metadata without payload bytes. Graph statistics and configuration are copied from the executed reconstruction.

The trace contains explainability data available to the engine. It intentionally excludes generation offsets/order, damage labels, pristine bytes, evaluator ground truth, and raw fragment contents.

```json
{"schema_version":1,"fragments":[{"id":0,"name":"part.bin","size":4}],"selected_path":[0],"mismatches":[],"repairs":[]}
```
