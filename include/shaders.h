#pragma once

#include <GLES2/gl2.h>

struct fwr_instance;

struct quad_rgbx_shader {
    GLuint prog;

    GLint proj;
    GLint tex;
    GLint alpha;
    GLint pos_attrib;
    GLint tex_attrib;
};

// Shader for rendering surfaces with anti-aliased rounded corner clipping.
// Uses SDF (Signed Distance Field) for smooth alpha falloff at corners.
struct quad_rounded_shader {
    GLuint prog;

    GLint tex;
    GLint alpha;
    GLint pos_attrib;
    GLint tex_attrib;

    // Clip rect in pixel coordinates (x, y, width, height)
    GLint clip_rect;
    // Corner radii: vec4(top-left, top-right, bottom-right, bottom-left)
    GLint corner_radii;
    // Output height for Y-flip correction
    GLint output_height;
};

struct quad_external_shader {
    GLuint prog;

    GLint tex;
    GLint pos_attrib;
    GLint tex_attrib;
};

struct quad_rgbx_shader make_quad_rgbx_shader(struct fwr_instance *instance);
struct quad_rounded_shader make_quad_rounded_shader(struct fwr_instance *instance);
struct quad_external_shader make_quad_external_shader(struct fwr_instance *instance);

extern const GLfloat quad_verts[8];