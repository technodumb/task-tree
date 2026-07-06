// The single translation unit that instantiates NanoVG's OpenGL 3 backend.
// glad must provide the GL symbols, so its loader header is included first.
#include <glad/gl.h>

#define NANOVG_GL3_IMPLEMENTATION
#include <nanovg.h>
#include <nanovg_gl.h>
