# CI and releases

- `ci.yml` builds and tests on Linux, macOS and Windows for every push to
  `main` and every pull request. It also runs a strict-C99 build with
  switch dispatch, and the tests under ASan and UBSan.
- `release.yml` runs when a `v*` tag is pushed. It builds and tests on the
  three platforms, packages `lvmi`, the static library, headers and docs,
  and publishes a GitHub release with the archives, `SHA256SUMS` and notes
  listing the commits since the previous tag. Tags containing `-` (for
  example `v0.2.0-rc1`) are marked as pre-releases.

```
git tag v0.1.0
git push origin v0.1.0
```
