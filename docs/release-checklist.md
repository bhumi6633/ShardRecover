# Release checklist

- [ ] Run `./scripts/certify.sh` from a clean working tree.
- [ ] Refresh `docs/benchmarks.md` on the release build and record the environment.
- [ ] Confirm the frontend production build and bundled sample trace.
- [ ] Review README, demo, trace-schema, fuzzing, and benchmark links.
- [ ] Capture optional demo screenshots without committing generated build output.
- [ ] Confirm `git status` is clean and CI is green.
- [ ] Create annotated tag `v0.1.0`.
- [ ] Push `main` and the tag, then draft the GitHub release from `CHANGELOG.md`.
