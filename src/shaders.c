#include "shaders.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <wlr/util/log.h>

const GLfloat quad_verts[8] = {
	1, -1, // top right
	-1, -1, // top left
	1, 1, // bottom right
	-1, 1, // bottom left
};

static const GLchar quad_vertex_src[] =
"attribute vec2 pos;\n"
"attribute vec2 texcoord;\n"
"varying vec2 v_texcoord;\n"
"\n"
"void main() {\n"
"\tgl_Position = vec4(pos, 1.0, 1.0);\n"
"\tv_texcoord = texcoord;\n"
"}\n";

static const GLchar tex_fragment_src_rgbx[] =
"precision mediump float;\n"
"varying vec2 v_texcoord;\n"
"uniform sampler2D tex;\n"
"\n"
"void main() {\n"
"  gl_FragColor = texture2D(tex, v_texcoord);\n"
"}\n";

static const GLchar tex_fragment_src_external[] =
"#extension GL_OES_EGL_image_external : require\n"
"precision mediump float;\n"
"varying vec2 v_texcoord;\n"
"uniform samplerExternalOES tex;\n"
"\n"
"void main() {\n"
"  gl_FragColor = texture2D(tex, v_texcoord);\n"
"}\n";

static const GLchar rounded_vertex_src[] =
"attribute vec2 pos;\n"
"attribute vec2 texcoord;\n"
"varying vec2 v_texcoord;\n"
"varying vec2 v_pos;\n"
"\n"
"void main() {\n"
"\tgl_Position = vec4(pos, 0.0, 1.0);\n"
"\tv_texcoord = texcoord;\n"
"\tv_pos = pos;\n"
"}\n";

static const GLchar rounded_fragment_src[] =
"precision highp float;\n"
"varying vec2 v_texcoord;\n"
"uniform sampler2D tex;\n"
"uniform float alpha;\n"
"uniform vec4 clip_rect;\n"
"uniform vec4 corner_radii;\n"
"uniform float output_height;\n"
"\n"
"float sdf_rounded_box(vec2 p, vec2 half_size, float r) {\n"
"  vec2 q = abs(p) - half_size + r;\n"
"  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"
"}\n"
"\n"
"void main() {\n"
"  vec2 frag_pos = vec2(gl_FragCoord.x, output_height - gl_FragCoord.y);\n"
"  vec2 rect_min = clip_rect.xy;\n"
"  vec2 rect_size = clip_rect.zw;\n"
"  vec2 rect_center = rect_min + rect_size * 0.5;\n"
"  vec2 half_size = rect_size * 0.5;\n"
"  vec2 local_pos = frag_pos - rect_center;\n"
"\n"
"  float r;\n"
"  if (local_pos.x < 0.0 && local_pos.y < 0.0) {\n"
"    r = corner_radii.x;\n"
"  } else if (local_pos.x >= 0.0 && local_pos.y < 0.0) {\n"
"    r = corner_radii.y;\n"
"  } else if (local_pos.x >= 0.0 && local_pos.y >= 0.0) {\n"
"    r = corner_radii.z;\n"
"  } else {\n"
"    r = corner_radii.w;\n"
"  }\n"
"\n"
"  float d = sdf_rounded_box(local_pos, half_size, r);\n"
"  float aa = smoothstep(0.5, -0.5, d);\n"
"\n"
"  vec4 color = texture2D(tex, v_texcoord);\n"
"  gl_FragColor = vec4(color.rgb, color.a * alpha * aa);\n"
"}\n";

GLuint compile_shader(struct fwr_instance *instance, GLuint type, const GLchar *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLchar *data = calloc(1, sizeof(GLchar) * 10000);
        GLsizei len = 0;
        glGetShaderInfoLog(shader, 10000, &len, data);

        printf("Shader compile error (%d): %.*s\n\n", len, len, data);
        printf("%s\n\n", src);

        free(data);
        glDeleteShader(shader);
        shader = 0;
    }

    return shader;
}

GLuint link_program(struct fwr_instance *instance, const GLchar *vert_src, const GLchar *frag_src) {
    GLuint vert = compile_shader(instance, GL_VERTEX_SHADER, vert_src);
    if (!vert) {
        printf("Failed to compile vertex shader!\n");
        return 0;
    }

    GLuint frag = compile_shader(instance, GL_FRAGMENT_SHADER, frag_src);
    if (!frag) {
        printf("Failed to compile fragment shader!\n");
        glDeleteShader(vert);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    glDetachShader(prog, vert);
    glDetachShader(prog, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE) {
        glDeleteProgram(prog);
        printf("Failed to link shader!\n");
        return 0;
    }

    return prog;
}

struct quad_rgbx_shader make_quad_rgbx_shader(struct fwr_instance *instance) {
    GLuint prog = link_program(instance, quad_vertex_src, tex_fragment_src_rgbx);

    struct quad_rgbx_shader shader = {};
    shader.prog = prog;
    shader.proj = glGetUniformLocation(prog, "proj");
    shader.tex = glGetUniformLocation(prog, "tex");
    shader.alpha = glGetUniformLocation(prog, "alpha");
    shader.pos_attrib = glGetAttribLocation(prog, "pos");
    shader.tex_attrib = glGetAttribLocation(prog, "texcoord");

    return shader;
}

struct quad_rounded_shader make_quad_rounded_shader(struct fwr_instance *instance) {
    GLuint prog = link_program(instance, rounded_vertex_src, rounded_fragment_src);

    struct quad_rounded_shader shader = {};
    shader.prog = prog;
    shader.tex = glGetUniformLocation(prog, "tex");
    shader.alpha = glGetUniformLocation(prog, "alpha");
    shader.pos_attrib = glGetAttribLocation(prog, "pos");
    shader.tex_attrib = glGetAttribLocation(prog, "texcoord");
    shader.clip_rect = glGetUniformLocation(prog, "clip_rect");
    shader.corner_radii = glGetUniformLocation(prog, "corner_radii");
    shader.output_height = glGetUniformLocation(prog, "output_height");

    return shader;
}

struct quad_external_shader make_quad_external_shader(struct fwr_instance *instance) {
    GLuint prog = link_program(instance, quad_vertex_src, tex_fragment_src_external);

    struct quad_external_shader shader = {};
    shader.prog = prog;
    shader.tex = glGetUniformLocation(prog, "tex");
    shader.pos_attrib = glGetAttribLocation(prog, "pos");
    shader.tex_attrib = glGetAttribLocation(prog, "texcoord");

    return shader;
}