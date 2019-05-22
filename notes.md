# Notes

A few random notes on various parts of the library.

* [Command lists](#command-lists)
* [Layers](#layers)
* [Clipping](#clipping)

## Command lists

Command lists are pre-recorded lists of commands (paths, strokes, fills, etc) which you can playback at any time by submitting the list to the context.

They can be useful in various cases. E.g for rendering multiple instances of the same group of paths or for conditional rendering, where the piece of code drawing the paths doesn't know if they will be actually shown on screen or not (the caller will decide at a later point).

Command lists can be cached (vg::CommandListFlags::Cacheable), meaning that the generated path tesselation will be cached the first time it's submitted and will be reused every time the command list is re-submitted (either during the same frame or at later frames).

A few notes regarding caching:
* It might be faster if your command list contains concave paths (compared to an uncached list). If all your paths are convex and you are rendering only a couple of instances, it might be slower than submitting the command list without turning on caching. 
* You can disable caching globally by setting VG_CONFIG_ENABLE_SHAPE_CACHING to 0.
* The tesselated geometry depends on the current scaling factor. The cache is invalidated if the current scaling doesn't match the scaling when the command list was cached.
* It's independent of translation and rotation. I.e. you can draw multiple instances of the same cached list using different transformations as long as scaling remains the same.
* Requires extra memory for the tesselated geometry.
* All commands, except path commands, are executed every time such a command list is submitted.
* Text cannot be cached. This is because the font atlas is dynamic and it's not guaranteed to be the same between frames.
* Resetting a cached command list is expensive.
* **ONLY** the tesselated paths are cached.

The best case scenario for using cached lists is for rendering multiple instances of static (w.r.t. their geometry) models with concave fills. Otherwise, caching might introduce extra overhead. Remember to use VG_CONFIG_ENABLE_SHAPE_CACHING to measure how much caching helps in your case and decide whether or not it's worth it.

Command lists can be nested, i.e. you can submit a command list (child) inside another command list (parent). If the child list is marked as transient (vg::CommandListFlags::Transient), its commands are copied into the parent list when calling clSubmitCommandList() and you are free to reuse (reset) the child list immediately. Otherwise, a SubmitCommandList command is inserted into the parent list and the child list is expected to stay valid until the parent is submitted to a layer.

## Layers

You can think of layers as multiple Contexts sharing the same images, fonts, gradients and image patterns.

* Each layer has its own state stack and its own bgfx::ViewId.
* The number of layers and their view IDs are specified at context creation time.
* Layers are submitted in order (from 0 to n).
* You are free to specify the same bgfx::ViewId for multiple layers. In this case layers can be used as a way to sort paths back to front.
* To change the current layer use setLayer() which returns the previous active layer. 
* Command lists cannot change the current layer. This means that if you are inside a beginCommandList()/endCommandList() block, calling setLayer() will assert if VG_CONFIG_DEBUG is enabled. Otherwise it will be silently ignored.

Since each layer has its own state stack (aka transformation matrix stack), all text measuring functions, which depend on the current transformation, take a layer ID as an extra argument. The same functions without a layer ID argument use the currently active layer.

## Clipping

TODO