# glgears (linuxonweb)

glxgears port for the wasm32 Linux guest: Mesa demos glxgears scene on
surfaceless EGL, drawn into an FBO and presented via XPutImage.

- `glgears.c`: the program (Brian Paul, public domain / MIT per file header).
- `build-glgears.sh`: static wasm32 link with Mesa (softpipe + Zink), the
  guest Vulkan ICD and Xlib. It is written for the linuxonweb build tree
  (`tools/zink/configure-wasm32.sh`), so it is not standalone.
- `glgears-s1-20261001.md`: the S1 evidence note from the project.

Pinned to the linuxonweb project commit 3de18d3 (not an upstream project).
