# vg-renderer

**WIP**

A vector graphics renderer for bgfx, based on ideas from both NanoVG and ImDrawList (Dear ImGUI)

Includes some small changes to FontStash.
Optionally uses libtess2 for concave polygon decomposition.

Based on NanoVG and FontStash versions included in bgfx repo.

### Path and Stroker classes

Paths are tesselated using the Path class (`src/vg/path.cpp, .h`). You can use this class to convert your SVG commands to a polyline without using the renderer interface.

Strokes and fills are generated using the Stroker class (`src/vg/stroker.cpp, .h). You can use this class to generate strokes and fills for your polylines without using the renderer interface.

### Compared to NanoVG/FontStash

1. Generates fewer draw calls, by batching multiple paths together, and using indexed triangle lists (like ImDrawList)
2. Separate shader programs for gradients to reduce uniform usage (NanoVG's bgfx backend uses a single program for all cases)
3. Circle() and RoundedRect() are implemented without Beziers
4. All textures are RGBA (even the font altas)
5. Concave polygons are decomposed into convex parts and rendered normally instead of using the stencil buffer (not tested extensively; might have issues with AA)
6. Stack-based Bezier tesselation
7. FontStash glyph hashing uses BKDR for better distribution of glyphs in the LUT (fewer collisions when searching for cached glyphs)
8. Shapes (aka prebaked command lists, aka display lists) with dynamic text support (i.e. the actual text string is retrieved at the time the shape is submitted for rendering, via a callback) (can be disabled with a compile-time flag (VG_SHAPE_DYNAMIC_TEXT)).
9. Caching of tessellated shapes.
10. FontStash caches the kerning info and glyph indices for ASCII chars in order to avoid repeatedly calling stbtt_XXX functions.
11. Clip in/out functionality using the stencil buffer.

### What's not supported compared to NanoVG

1. Miter limit
2. Arbitrary polygon winding
3. Polygon holes
4. Variable text line height
5. Skew transformation matrix

### Comparison screenshots

The code is currently used in [DLS](http://makingartstudios.itch.io/dls) for rendering both the schematic and the UI. Below are two screenshots comparing NanoVG's output with vg-renderer's output.

[![NanoVG](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/i8080_nanovg.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/i8080_nanovg.png)
[![vg-renderer](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/i8080_vg_renderer.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/i8080_vg_renderer.png)
