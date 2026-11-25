# coredcc

A C++20 foundation library for 3D content creation tools (DCCs), built on
[litestl](https://github.com/joeedh/litestl).

It is meant to run in these places:
- as WASM in a browser or a WebGPU frontend;
- as a native N-API addon in NW.js or Chromium (planned, P6);
- linked into a native editor, such as Unreal's (planned, P4).

## What is in it

- **Documents:** blocks with stable ids, references, garbage collection, attachments and files.
- **Properties and datapaths:** metadata with units, paths such as
  `objects[0].transform.location`, and notifications of changes.
- **Commands:** undoable, with an undo history that a host editor can replace.
- **Layouts:** descriptions of a UI, as XML or from C++, which a frontend draws.
- **Objects and scenes:** pluggable transform models (TRS, FBX and matrix are built in), evaluation,
  and picking.
- **A BVH** over triangles, capsules, spheres or a client's own primitives.
- **A dependency graph:** node types, groups, foreach, and incremental evaluation.
- **Scripts:** TypeScript bindings, in which commands, transform types, graph nodes and other
  interfaces can be written.

Interactive tools, gizmos, overlay rendering and the scene evaluator that drives objects from a graph
aren't there yet. See the [task list](docs/implementation-tasklist.md).

## Building

Windows only so far: Visual Studio 18 with clang-cl.

```
git clone --recurse-submodules https://github.com/joeedh/coredcc.git
cd coredcc
tools\devshell.cmd cmake --preset native-clang
tools\devshell.cmd cmake --build --preset native-clang-release -j 5
tools\devshell.cmd ctest --preset native-clang-release
```

[docs/building.md](docs/building.md) has the rest: prerequisites, the other presets, WASM, the gates, the
test app and the benchmarks.

## Documentation

Start at [docs/index.md](docs/index.md).

## State

In development, with no releases. APIs change without notice.

## License

[MIT](LICENSE)
