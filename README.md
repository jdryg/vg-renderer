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
10. Per-fill and per-stroke control over whether anti-aliasing geometry should be generated.

### What's not supported compared to NanoVG

1. Miter limit (i.e miter joins never convert to bevel joins)
2. Polygon holes (they can be emulated using clip in/out regions)
3. Variable text line height, font blur effect, and letter-spacing control
4. Skew transformation matrix

### Example

For an example on how to use this library, take a look [here](https://github.com/jdryg/bgfx/blob/xx-vg-renderer/examples/xx-vg-renderer/vg-renderer.cpp).

### A few tips when porting from NanoVG.

 - `nvgBezierTo` should be replaced by `vg::cubicTo`. Note that there is also `vg::quadraticTo` for a lower degree Bezier curve.
 - `nvgSave` and `nvgRestore` are to be replaced by `vg::pushState()` and `vg::popState()`.
 - Thanks to the support for command lists, you may want to render your UI in layers, where every layer is a command list.
   This way, you can still traverse the UI-tree widget by widget, but store draw commands onto the command lists per layer, which can greatly reduce state changes.
   For example you may have a layer for widget shapes and outlines, and a layer for text.
 - When implementing UI widget with perfectly rectangular shape, you may consider omitting the anti-aliasing setting to generate less geometry.
 - When using a gradient fill that will be fully transparent at the edge of the shape, you may likely also want to not generate anti-aliasing geometry.
 - vg-renderer uses `uint32_t` as colors, instead of a struct of 4 floats.
   When converting, you might want to consider to use `vg::color4ub` instead of `vg::color4f`.
   Also note that there are a few predefined colors available in `vg::Colors::`.


### Images

[DLS](https://makingartstudios.itch.io/dls)

[![DLS](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/dls.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/dls.png)

SVG (using [simple-svg](https://github.com/jdryg/simple-svg))

[![Tiger](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_tiger.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_tiger.png)

Demo

[![Demo](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_demo.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_demo.png)

Custom gradients (indexed triangle lists w/ per-vertex colors)

[![Gradients](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_colorwheel.png)](https://raw.githubusercontent.com/jdryg/vg-renderer/master/img/vgrenderer_colorwheel.png)

### Using this in your project

In your project, you can add these files as a new CMake target, using the following, assuming you have this project as a submodule in a folder `ext/vg-renderer`:

```cmake
add_library(vg-renderer STATIC
    "ext/vg-renderer/src/vg.cpp"
    "ext/vg-renderer/src/stroker.cpp"
    "ext/vg-renderer/src/path.cpp"
    "ext/vg-renderer/src/vg_util.cpp"
    "ext/vg-renderer/src/libs/fontstash.cpp"
    "ext/vg-renderer/src/libs/stb_truetype.cpp"

    "ext/vg-renderer/src/libtess2/bucketalloc.c"
    "ext/vg-renderer/src/libtess2/dict.c"
    "ext/vg-renderer/src/libtess2/geom.c"
    "ext/vg-renderer/src/libtess2/mesh.c"
    "ext/vg-renderer/src/libtess2/priorityq.c"
    "ext/vg-renderer/src/libtess2/sweep.c"
    "ext/vg-renderer/src/libtess2/tess.c"
    )
target_include_directories(vg-renderer PUBLIC "ext/vg-renderer/include/")
# Add the bgfx and bx libraries to link with this target:
# I made a macro in my project that does this. You can roll your own.
link_bgfx_libs(vg-renderer Release)
```
