# Third-party licenses / SBOM

thyme links no third-party libraries beyond system libraries. One vendored
asset is shipped as generator scaffolding: a minified ECharts build used by the
optional offline visualization page.

## Runtime and build dependencies

| Dependency | Purpose | License | Delivery |
|---|---|---|---|
| libc / libstdc++ (or libc++) | Standard library, POSIX | System toolchain license | Provided by the target system |
| pthreads | Threads / condition variables | System toolchain license | Provided by the target system |
| Linux futex/rt | Cross-process waits | Kernel/system | Provided by the target system |
| CMake | Build | BSD-3-Clause | Build-machine tool, not shipped |
| python3 | Test/CI scripts (`tools/gen/gen_contracts`, `tools/ci`) | PSF-2.0 | Build-machine tool, not shipped |
| ws2_32 / winsock2 / Windows SDK | Windows-specific socket and I/O branches (`thyme_bin_stream.cpp`, `mcap_writer.cpp`) | Windows SDK system license | Windows targets only; provided by the system |
| ECharts (minified `echarts.min.js`) | Optional offline dashboard rendering in generated scaffold projects | Apache-2.0 | Vendored asset under `tools/gen/gen_graph/scaffold/tools/`; copied into generated projects |

> If zstd/lz4, MCAP, Python bindings, or other dependencies are added later,
> record the name, version, license, and usage location in this table and list
> them in `NOTICE` before merging.

## SBOM

`tools/ci/gen_sbom.sh` generates CycloneDX 1.5 JSON (CycloneDX-like, offline,
with no new dependencies; it requires only bash and python3):

```bash
tools/ci/gen_sbom.sh --output sbom.json     # write to a file
tools/ci/gen_sbom.sh > sbom.json            # or write to stdout
tools/ci/gen_sbom.sh --timestamp            # add metadata.timestamp (omitted by default)
```

The dependency list is derived from the repository rather than hardcoded:

1. `CMakeLists.txt`: `project(... VERSION ...)`, `find_package(Threads)`, `target_link_libraries(... rt)`, and `python3`;
2. System headers included by `src/` and `include/`, classified as libc / libstdc++ / pthreads / Linux UAPI / Winsock; an unclassified header fails the script;
3. The table above supplies each component's `licenses`.

**Dependency-depth check:** a dependency that appears in the build but is not
registered in this table makes the script fail with exit code 2 and list the
evidence location. `--allow-unknown` is for local debugging only (missing rows
are reported as `NOASSERTION`) and must not be used in CI or releases.

> The GitHub Actions workflow calls
> `tools/ci/gen_sbom.sh --output sbom.json` without `--allow-unknown`.

## Known gaps

- ECharts is a vendored asset rather than a linked dependency; the SBOM script
  covers build dependencies, so its Apache-2.0 notice is recorded here rather
  than generated from `#include` directives.
- The SBOM covers direct dependencies only; it does not expand system-library
  internals.
