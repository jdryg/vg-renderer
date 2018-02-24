# vg-renderer

A vector graphics renderer for bgfx, based on ideas from NanoVG and ImDrawList (Dear ImGUI)

Includes some small changes to FontStash. Optionally uses libtess2 for concave polygon decomposition.

### Path and Stroker classes

Paths are tesselated using the Path struct (`src/vg/path.cpp, .h`). You can use vg::pathXXX() functions to convert your SVG commands to a polyline for uses outside this renderer.

Strokes and fills are generated using the Stroker struct (`src/vg/stroker.cpp, .h`). You can use vg::strokerXXX() functions to generate strokes and fills for your polylines for uses outside this renderer.

### Compared to NanoVG/FontStash

1. Generates fewer draw calls by batching multiple paths together (if they share the same state)
2. Separate shader programs for gradients and image patterns to reduce uniform usage (NanoVG's bgfx backend uses a single program for all cases)
3. Concave polygons are decomposed on the CPU.
4. Command lists with support for tesselation caching.
5. Clip in/out with the stencil buffer
6. User-specified indexed triangle list rendering (i.e. for complex gradients and sprite atlases).
7. Fills with image patterns can be colored.
8. FontStash: glyph hashing uses BKDR (seems to give better distribution of glyphs in the LUT; fewer collisions when searching for cached glyphs)
9. FontStash: optional (compile-time flag) caching of glyph indices and kerning info for ASCII chars in order to avoid repeated calls to stbtt functions.

### What's not supported compared to NanoVG

1. Miter limit (i.e miter joins never convert to bevel joins)
2. Polygon holes (they can be emulated using clip in/out regions)
3. Variable text line height
4. Skew transformation matrix

### Images

[DLS](https://makingartstudios.itch.io/dls)

[![DLS](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/dls.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/dls.png)

SVG (using [simple-svg](https://github.com/jdryg/simple-svg))

[![Tiger](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_tiger.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_tiger.png)

Demo

[![Demo](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_demo.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_demo.png)

Custom gradients (indexed triangle lists w/ per-vertex colors)

[![Gradients](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_colorwheel.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_colorwheel.png)