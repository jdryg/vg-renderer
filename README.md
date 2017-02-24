# vg-renderer

A vector graphics renderer for bgfx, based on ideas from both NanoVG and ImDrawList (Dear ImGUI)

**WIP** and doesn't compile as is! Requires structs and functions from my codebase which aren't present in this repo. Nothing too complicated (it needs a Vec2 struct and several math/utility functions) so if you really want to make it compile, you can try writing/using your own version of those.

Also includes some small changes to FontStash.

Based on NanoVG and FontStash included in bgfx.

Compared to NanoVG/FontStash:

1. Generates fewer draw calls, by batching multiple paths together, and indexed triangle lists (ala ImDrawList)
2. Separate shader program for gradients to reduce uniform usage (NanoVG's bgfx backend uses a single program for all cases)
3. Circle() is implemented without Beziers (there's a switch to use the original NanoVG code)
4. All textures are RGBA (even the font altas)
5. Concave polygons are decomposed into convex parts and rendered normally instead of using the stencil buffer (not tested extensively) (uses algorithm from https://mpen.ca/406/bayazit)
6. Stack-based Bezier tesselation
7. FontStash glyph hashing uses BKDR for better distribution of glyphs in the LUT (fewer collisions when searching for cached glyphs)

What's not supported compared to NanoVG

1. Round caps and joins
2. Bevel joins
