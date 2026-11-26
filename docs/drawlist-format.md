# DrawList format, version 1

A DrawList (`source/coredcc/draw/drawlist.h`) is what coredcc's tools and objects draw over a viewport. A host
renders it once a frame. It is plain bytes, so a host reads it without a call per primitive: a view of WASM memory,
or one copy. `DrawListReader` (C++) and `bind/ts/drawlist.ts` (TS) read it and check it.

## Rules for hosts
- Take the bytes again every frame: `bytes()` in C++; `address()` and `size()`, or `read()`, in scripts. Never keep a
  typed-array view: the list's storage moves when it grows, and so does WASM memory.
- The list is frozen between `end()` and the next `begin()`. It is read in that time only.
- Check it before rendering it if it came from somewhere you don't trust (`DrawListReader::read`).
- Unknown versions are refused. A newer version may add kinds; a reader of this version refuses them.

## Byte order and alignment
- Little-endian. Floats are IEEE 754 binary32, doubles binary64.
- The list starts on an 8-byte boundary, and every record's size is a multiple of 8, so every record starts on one.
  Readers still read fields with unaligned loads: a copy may not be aligned.

## Header (48 bytes)
| Offset | Type | Field |
|---|---|---|
| 0 | u32 | magic `0x4c444443` (the bytes `CDDL`) |
| 4 | u16 | version, 1 |
| 6 | u16 | flags, 0 |
| 8 | u32 | the number of records |
| 12 | u32 | the size of the whole list in bytes, header included |
| 16 | 3 × f64 | the origin |
| 40 | u64 | reserved, 0 |

## Records
Every record starts with 24 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | kind (below) |
| 2 | u8 | depth mode: 0 test, 1 always (no depth test), 2 x-ray |
| 3 | u8 | flags: 1 a color per corner (Triangles), 2 filled (Circle2D) |
| 4 | u32 | the record's size in bytes, header included, a multiple of 8 |
| 8 | u32 | color: RGBA8, straight alpha, red in the lowest byte |
| 12 | f32 | width: of lines and outlines in logical pixels; the diameter of points |
| 16 | u32 | id: what the record draws (a gizmo's part: the gizmo's id × 65,536 + the part); 0 for none |
| 20 | u32 | aux: the vertex count of Lines, Points, Triangles and Line2D; the shape of a Shape |

The payload follows at offset 24, padded with zeros to a multiple of 8.

| Kind | Number | Payload |
|---|---|---|
| Lines | 1 | aux × 3 f32 (x, y, z); two vertices per segment, so aux is even |
| Points | 2 | aux × 3 f32 |
| Triangles | 3 | aux × 3 f32, three per triangle; then, with flag 1, aux × u32 colors |
| Shape | 4 | 16 f64: a column-major matrix from the shape's space |
| Circle2D | 5 | 3 f32: x, y and the radius in logical pixels |
| Line2D | 6 | aux × 2 f32: a strip of points (x, y) |
| PushMatrix | 7 | 16 f64: a column-major, affine matrix |
| PopMatrix | 8 | none |

The style fields of PushMatrix and PopMatrix are unused.

## Coordinates and precision
- **3D records are in the current space.** The stack starts empty: the current space is the world, less the origin.
  `PushMatrix m` makes the current matrix `current · m`; `PopMatrix` goes back. Pushes and pops balance in a list.
- **Outside any push,** a point is stored as `float(p - origin)`, and a Shape's or the outermost PushMatrix's matrix
  has its translation stored minus the origin, in double. **Inside a push,** points are the floats of the pushed space,
  and matrices are stored as given.
- So a point's float is relative to the origin: precision is best near it and falls with the distance from it.
  A host computes its view-projection relative to the origin in double, `VP · T(origin)`, and multiplies the
  pushed matrices into it in double before it hands floats to the GPU.
- Matrices are column-major and apply to column vectors (as coredcc's `mat4d`): the translation is elements 12 to 14.
- **2D records** are in device pixels, in the same space as `View::viewport` and pointer events, with y down. Their
  widths and radii are in logical pixels: multiply by `View::dpiScale`.
- coredcc's tools set the origin to the camera's position, where what is drawn largest is, unless the host sets
  `ToolManager::drawOrigin` (a floating origin, say).

## Depth modes
- **Test:** drawn where it is in front of the depth buffer.
- **Always:** drawn over everything.
- **X-ray:** drawn twice: tested at full color, then where it is hidden at about a third of its alpha.

## Shapes
Each is defined in its own space, about one unit in size, along +z where it has a direction. Hosts make meshes of them
with enough segments to look round at a gizmo's size (24 around is enough), and coredcc's gizmos hit-test the same
geometry.

| Shape | Number | Geometry |
|---|---|---|
| Arrow | 1 | a cylinder of radius 0.02 from z = 0 to 0.8, and a cone of radius 0.07 from z = 0.8 to 1 |
| Cone | 2 | a cone of radius 0.5 from z = 0 (the base) to 1 (the tip) |
| Cube | 3 | the cube from -0.5 to 0.5 on each axis |
| Sphere | 4 | the sphere of radius 0.5 about the origin |
| Disc | 5 | the filled disc of radius 1 in the z = 0 plane |
| Ring | 6 | a torus about the z axis: major radius 1, minor radius 0.02 |
| Quad | 7 | the square from -0.5 to 0.5 in x and y, at z = 0 |

Shapes are filled, in the record's color. Lines and outlines of shapes are drawn as separate records.

## Errors
A list that breaks any rule above is invalid. `DrawListReader` (C++) and `decodeDrawList` (TS) check the same rules,
and refuse it naming the byte offset:
- the header: its magic, version, flags (0), size (the list's) and reserved word (0); an origin that isn't finite;
- a record header past the end; a kind, depth mode or flag that this kind doesn't have; a width that is negative or
  isn't finite;
- a record size that isn't a multiple of 8, is past the end, or doesn't fit the count (Lines' even, Triangles' a
  multiple of 3; a Shape's shape exists);
- padding that isn't zeros;
- a number that isn't finite; a Shape's or PushMatrix's matrix that isn't affine; a Circle2D's negative radius;
- a pop without a push, or pushes left at the end; a record count that isn't the header's.

Writers refuse to make one: `DrawList`'s appends return InvalidArgument and append nothing.

## Record ids
A gizmo's records have the id `gizmo id × 65,536 + part`. ToolManager gives each gizmo the smallest free id, and at
most 65,535 of them at once, so every id fits in 32 bits. Hosts that pick by id read it back the same way.
