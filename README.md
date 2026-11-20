# graph-lint

A topology linter for `cudaGraph_t`: it will report depth, width per level, redundant edges and
synchronization nodes, with a JSON contract and the exit codes a CI can gate on.

Status: pre-release, API unstable. This tree is the scaffold only; the graph model, the JSON schema
and the CUDA source are not here yet.

    build-host.bat configure
    build-host.bat build
    build-host.bat test
