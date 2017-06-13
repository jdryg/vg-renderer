# vg-renderer

A vector graphics renderer for bgfx, based on ideas from both NanoVG and ImDrawList (Dear ImGUI)

**WIP** and doesn't compile as is! Requires structs and functions from my codebase which aren't present in this repo. Nothing too complicated (it needs a Vec2 struct and several math/utility functions) so if you really want to make it compile, you can try writing/using your own version of those.

Also includes some small changes to FontStash.

Based on NanoVG and FontStash versions included in bgfx repo.

### Compared to NanoVG/FontStash

1. Generates fewer draw calls, by batching multiple paths together, and using indexed triangle lists (like ImDrawList)
2. Separate shader program for gradients to reduce uniform usage (NanoVG's bgfx backend uses a single program for all cases)
3. Circle() and RoundedRect() are implemented without Beziers
4. All textures are RGBA (even the font altas)
5. Concave polygons are decomposed into convex parts and rendered normally instead of using the stencil buffer (not tested extensively; might have issues with AA) (uses algorithm from https://mpen.ca/406/bayazit)
6. Stack-based Bezier tesselation
7. FontStash glyph hashing uses BKDR for better distribution of glyphs in the LUT (fewer collisions when searching for cached glyphs)
8. Shapes (aka prebaked command lists, aka display lists) with dynamic text support (i.e. the actual text string is retrieved at the time the shape is submitted for rendering, via a callback) (can be disabled with a compile-time flag (VG_SHAPE_DYNAMIC_TEXT)).
9. Caching of tessellated shapes.

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
