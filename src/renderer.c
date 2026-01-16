#include "renderer.h"
#include "flutter_embedder.h"
#include "instance.h"
#include "shaders.h"
#include <EGL/egl.h>
#include <wlr/render/gles2.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <wayland-util.h>

#include <wlr/render/egl.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <pixman.h>

#include "EGL/egl.h"

#define GL_BGRA_EXT 0x80E1

static const GLfloat texcoords[8] = {
  1.0f, 1.0f,
  0.0f, 1.0f,
  1.0f, 0.0f,
  0.0f, 0.0f,
};

static void texture_destruction_callback(void *user_data) {}

static struct fwr_renderer_page_texture* page_get_texture(struct fwr_instance *instance, size_t width, size_t height, bool make_fbo) {
  struct fwr_renderer *renderer = &instance->fwr_renderer;
  struct gl_fns *fns = &renderer->fns;
  struct fwr_renderer_page *page = &renderer->pages[renderer->current_page];

  bool found = false;
  struct fwr_renderer_page_texture *page_texture;
  wl_list_for_each(page_texture, &page->unused_textures, link) {
    if (page_texture->width == width && page_texture->height == height) {
      found = true;
      break;
    }
  }

  if (found) {
    wl_list_remove(&page_texture->link);
  } else {
    GLuint err;
    GLuint fbo = 0;

    if (make_fbo) {
      fns->glGenFramebuffers(1, &fbo);
      wlr_log(WLR_DEBUG, "fbo: %d", fbo);

      fns ->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }

    GLuint tex = 0;
    fns->glGenTextures(1, &tex);
    wlr_log(WLR_DEBUG, "tex: %d", tex);

    fns->glBindTexture(GL_TEXTURE_2D, tex);
    fns->glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
    fns->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    fns->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if (make_fbo) {
      fns->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

      GLenum drawBuffers[1] = {GL_COLOR_ATTACHMENT0};
      fns->glDrawBuffers(1, drawBuffers);

      fns->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    fns->glBindTexture(GL_TEXTURE_2D, 0);

    err = fns->glGetError();
    if (err != 0) {
      wlr_log(WLR_ERROR, "GL ERROR %d", err);
    }

    page_texture = calloc(1, sizeof(struct fwr_renderer_page_texture));
    page_texture->page = page;
    page_texture->texture = tex;
    page_texture->fbo = fbo;
    page_texture->width = width;
    page_texture->height = height;
  }

  wl_list_insert(&page->textures, &page_texture->link);

  return page_texture;
}

static bool create_backing_store(const FlutterBackingStoreConfig *config, FlutterBackingStore *backing_store_out, void *user_data) {
  struct fwr_instance *instance = user_data;

  struct fwr_renderer_page_texture *page_texture = page_get_texture(instance, config->size.width, config->size.height, false);

  backing_store_out->struct_size = sizeof(FlutterBackingStore);
  backing_store_out->user_data = page_texture;
  backing_store_out->type = kFlutterBackingStoreTypeOpenGL;
  backing_store_out->did_update = false;

  backing_store_out->open_gl.type = kFlutterOpenGLTargetTypeTexture;
  backing_store_out->open_gl.texture.target = GL_TEXTURE_2D;
  backing_store_out->open_gl.texture.name = page_texture->texture;
  backing_store_out->open_gl.texture.format = 0x93A1;
  backing_store_out->open_gl.texture.user_data = page_texture;
  backing_store_out->open_gl.texture.destruction_callback = texture_destruction_callback;
  backing_store_out->open_gl.texture.width = page_texture->width;
  backing_store_out->open_gl.texture.height = page_texture->height;

  return true;
}

static bool collect_backing_store(const FlutterBackingStore *backing_store, void *user_data) {
  return true;
}

static bool present_layers(const FlutterLayer** f_layers, size_t layers_count, void *user_data) {
  struct fwr_instance *instance = user_data;
  struct fwr_renderer *renderer = &instance->fwr_renderer;

  pthread_mutex_lock(&renderer->render_mutex);

  for (int i = 0; i < renderer->current_scene.layers_count; i++) {
    struct fwr_renderer_scene_layer *layer = &renderer->current_scene.layers[i];
    if (layer->type == sceneLayerPlatform) {
      free(layer->platform.mutations);
    }
  }
  free(renderer->current_scene.layers);

  struct fwr_renderer_scene_layer *layers = calloc(layers_count, sizeof(struct fwr_renderer_scene_layer));

  for (int i = 0; i < layers_count; i++) {
    const FlutterLayer *f_layer = f_layers[i];
    struct fwr_renderer_scene_layer *layer = &layers[i];

    layer->offset = f_layer->offset;
    layer->size = f_layer->size;

    if (f_layer->type == kFlutterLayerContentTypeBackingStore) {
      layer->type = sceneLayerTexture;
      layer->texture.texture = f_layer->backing_store->user_data;
    } else if (f_layer->type == kFlutterLayerContentTypePlatformView) {
      const FlutterPlatformView *platform_view = f_layer->platform_view;

      layer->type = sceneLayerPlatform;
      layer->platform.platform_view_id = platform_view->identifier;
      layer->platform.mutations_count = platform_view->mutations_count;
      
      FlutterPlatformViewMutation *mutations = calloc(platform_view->mutations_count, sizeof(FlutterPlatformViewMutation));
      for (int k = 0; k < platform_view->mutations_count; k++) {
        const FlutterPlatformViewMutation *f_mutation = platform_view->mutations[k];
        mutations[k] = *f_mutation;
      }
      layer->platform.mutations = mutations;
    }
  }

  renderer->current_scene.layers_count = layers_count;
  renderer->current_scene.layers = layers;
  renderer->current_scene.needs_update = true;

  uint8_t last_page_idx;
  if (renderer->current_page == 0) {
    last_page_idx = 1;
  } else {
    last_page_idx = 0;
  }

  struct fwr_renderer_page *page = &renderer->pages[renderer->current_page];
  struct fwr_renderer_page_texture *page_texture;
  struct fwr_renderer_page_texture *tmp;
  wl_list_for_each_safe(page_texture, tmp, &page->unused_textures, link) {
    renderer->fns.glDeleteTextures(1, &page_texture->texture);
    wl_list_remove(&page_texture->link);
    free(page_texture);
  }

  if (renderer->current_scene.sync != 0) {
    eglDestroySync(instance->egl_display, renderer->current_scene.sync);
  }

  renderer->current_scene.sync = eglCreateSync(instance->egl_display, EGL_SYNC_FENCE, NULL);

  struct fwr_renderer_page *last_page = &renderer->pages[last_page_idx];
  wl_list_for_each_safe(page_texture, tmp, &last_page->textures, link) {
    wl_list_remove(&page_texture->link);
    wl_list_insert(&last_page->unused_textures, &page_texture->link);
  }

  if (renderer->current_page == 0) {
    renderer->current_page = 1;
  } else {
    renderer->current_page = 0;
  }

  pthread_mutex_unlock(&renderer->render_mutex);
  return true;
}

void fwr_renderer_init(struct fwr_instance *instance, gl_resolve_fn resolver) {
  struct fwr_renderer *renderer = &instance->fwr_renderer;
  struct gl_fns *fns = &renderer->fns;

  fns->glGenFramebuffers = (void (*)(GLsizei, GLuint*)) resolver("glGenFramebuffers");
  fns->glBindFramebuffer = (void (*)(GLenum, GLuint)) resolver("glBindFramebuffer");
  fns->glGenTextures = (void (*)(GLsizei, GLuint*)) resolver("glGenTextures");
  fns->glBindTexture = (void (*)(GLenum, GLuint)) resolver("glBindTexture");
  fns->glTexImage2D = (void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) resolver("glTexImage2D");
  fns->glTexParameteri = (void (*)(GLenum, GLenum, GLint)) resolver("glTexParameteri");
  fns->glFramebufferTexture = (void (*)(GLenum, GLenum, GLuint, GLint)) resolver("glFramebufferTexture");
  fns->glDrawBuffers = (void (*)(GLsizei, const GLenum*)) resolver("glDrawBuffers");
  fns->glCreateShader = (GLuint (*)(GLenum)) resolver("glCreateShader");
  fns->glShaderSource = (void (*)(GLuint, GLsizei, const GLchar**, const GLint*)) resolver("glShaderSource");
  fns->glCompileShader = (void (*)(GLuint)) resolver("glCompileShader");
  fns->glGetShaderiv = (void (*)(GLuint, GLenum, GLint*)) resolver("glGetShaderiv");
  fns->glDeleteShader = (void (*)(GLuint)) resolver("glDeleteShader");
  fns->glCreateProgram = (GLuint (*)()) resolver("glCreateProgram");
  fns->glAttachShader = (void (*)(GLuint, GLuint)) resolver("glAttachShader");
  fns->glLinkProgram = (void (*)(GLuint)) resolver("glLinkProgram");
  fns->glDetachShader = (void (*)(GLuint, GLuint)) resolver("glDetachShader");;
  fns->glGetProgramiv = (void (*)(GLuint, GLenum, GLint*)) resolver("glGetProgramiv");
  fns->glDeleteProgram = (void (*)(GLuint)) resolver("glDeleteProgram");
  fns->glGetUniformLocation = (GLint (*)(GLuint, const GLchar*)) resolver("glGetUniformLocation");
  fns->glGetAttribLocation = (GLint (*)(GLuint, const GLchar*)) resolver("glGetAttribLocation");
  fns->glActiveTexture = (void (*)(GLenum)) resolver("glActiveTexture");
  fns->glUseProgram = (void (*)(GLuint)) resolver("glUseProgram");
  fns->glUniformMatrix3fv = (void (*)(GLint, GLsizei, GLboolean, const GLfloat*)) resolver("glUniformMatrix3fv");
  fns->glUniform1i = (void (*)(GLint, GLint)) resolver("glUniform1i");
  fns->glUniform1f = (void (*)(GLint, GLfloat)) resolver("glUniform1f");
  fns->glUniform2f = (void (*)(GLint, GLfloat, GLfloat)) resolver("glUniform2f");
  fns->glUniform4f = (void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat)) resolver("glUniform4f");
  fns->glVertexAttribPointer = (void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) resolver("glVertexAttribPointer");
  fns->glEnableVertexAttribArray = (void (*)(GLuint)) resolver("glEnableVertexAttribArray");
  fns->glDrawArrays = (void (*)(GLenum, GLint, GLsizei)) resolver("glDrawArrays");
  fns->glDisableVertexAttribArray = (void (*)(GLuint)) resolver("glDisableVertexAttribArray");
  fns->glEnable = (void (*)(GLenum)) resolver("glEnable");
  fns->glDisable = (void (*)(GLenum)) resolver("glDisable");
  fns->glGetTextureImage = (void (*)(GLuint, GLint, GLenum, GLenum, GLsizei, void*)) resolver("glGetTextureImage");
  fns->glCheckFramebufferStatus = (GLenum (*)(GLenum)) resolver("glCheckFramebufferStatus");
  fns->glGetError = (GLenum (*)()) resolver("glGetError");
  fns->glFramebufferTexture2D = (void (*)(GLenum, GLenum, GLenum, GLuint, GLint)) resolver("glFramebufferTexture2D");
  fns->glGenBuffers = (void (*)(GLsizei, GLuint*)) resolver("glGenBuffers");
  fns->glBindBuffer = (void (*)(GLenum, GLuint)) resolver("glBindBuffer");
  fns->glBufferData = (void (*)(GLenum, GLsizeiptr, const void*, GLenum)) resolver("glBufferData");
  fns->glClearColor = (void (*)(GLfloat, GLfloat, GLfloat, GLfloat)) resolver("glClearColor");
  fns->glClear = (void (*)(GLbitfield)) resolver("glClear");
  fns->glDeleteFramebuffers = (void (*)(GLsizei, GLuint*)) resolver("glDeleteFramebuffers");
  fns->glDeleteTextures = (void (*)(GLsizei, GLuint*)) resolver("glDeleteTextures");
  fns->glBindSampler = (void (*)(GLuint, GLuint)) resolver("glBindSampler");
  fns->glBlendFuncSeparate = (void (*)(GLenum, GLenum, GLenum, GLenum)) resolver("glBlendFuncSeparate");

  fns->glGetIntegerv = (void (*)(GLenum, GLint*)) resolver("glGetIntegerv");
  fns->glGetBooleanv = (void (*)(GLenum, GLboolean*)) resolver("glGetBooleanv");
  fns->glReadPixels = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) resolver("glReadPixels");
  fns->glViewport = (void (*)(GLint, GLint, GLsizei, GLsizei)) resolver("glViewport");

  const char *egl_exts = eglQueryString(instance->egl_display, EGL_EXTENSIONS);
  bool ext_context_priority = strstr(egl_exts, "EGL_IMG_context_priority") != NULL;
  bool ext_context_robustness = strstr(egl_exts, "EGL_EXT_create_context_robustness") != NULL;

  EGLint client_version = 2;
  eglQueryContext(instance->egl_display, instance->egl_context, EGL_CONTEXT_CLIENT_VERSION, &client_version);

  EGLint context_priority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
  bool has_context_priority = false;
  if (ext_context_priority && eglQueryContext(instance->egl_display, instance->egl_context, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &context_priority)) {
    has_context_priority = true;
  }

  EGLint reset_strategy = EGL_LOSE_CONTEXT_ON_RESET_EXT;
  bool has_reset_strategy = false;
  if (ext_context_robustness && eglQueryContext(instance->egl_display, instance->egl_context, EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT, &reset_strategy)) {
    has_reset_strategy = true;
  }

  EGLint renderable_type = EGL_OPENGL_ES2_BIT;
#if defined(EGL_OPENGL_ES3_BIT)
  if (client_version >= 3) {
    renderable_type = EGL_OPENGL_ES3_BIT;
  }
#elif defined(EGL_OPENGL_ES3_BIT_KHR)
  if (client_version >= 3) {
    renderable_type = EGL_OPENGL_ES3_BIT_KHR;
  }
#endif

  size_t atti = 0;
  EGLint flutter_context_attribs[9];
  flutter_context_attribs[atti++] = EGL_CONTEXT_CLIENT_VERSION;
  flutter_context_attribs[atti++] = client_version;
  if (has_context_priority) {
    flutter_context_attribs[atti++] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
    flutter_context_attribs[atti++] = context_priority;
  }
  if (has_reset_strategy) {
    flutter_context_attribs[atti++] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT;
    flutter_context_attribs[atti++] = reset_strategy;
  }
  flutter_context_attribs[atti++] = EGL_NONE;

  EGLConfig flutter_config = EGL_NO_CONFIG_KHR;
  renderer->flutter_egl_context = eglCreateContext(
    instance->egl_display, EGL_NO_CONFIG_KHR, instance->egl_context, flutter_context_attribs);

  if (renderer->flutter_egl_context == EGL_NO_CONTEXT) {
    EGLint egl_error = eglGetError();
    wlr_log(WLR_INFO, "Configless context failed (0x%x), trying explicit config", egl_error);

    EGLConfig egl_config = EGL_NO_CONFIG_KHR;
    EGLint num_configs = 0;
    EGLint egl_config_id = 0;

    if (eglQueryContext(instance->egl_display, instance->egl_context, EGL_CONFIG_ID, &egl_config_id)) {
      EGLint config_id_attribs[] = {
        EGL_CONFIG_ID, egl_config_id,
        EGL_NONE
      };

      if (eglChooseConfig(instance->egl_display, config_id_attribs, &egl_config, 1, &num_configs) && num_configs > 0) {
        renderer->flutter_egl_context = eglCreateContext(
          instance->egl_display, egl_config, instance->egl_context, flutter_context_attribs);
        if (renderer->flutter_egl_context != EGL_NO_CONTEXT) {
          flutter_config = egl_config;
        }
      }
    }

    if (renderer->flutter_egl_context == EGL_NO_CONTEXT) {
      EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, renderable_type,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
      };

      if (eglChooseConfig(instance->egl_display, config_attribs, &egl_config, 1, &num_configs) && num_configs > 0) {
        renderer->flutter_egl_context = eglCreateContext(
          instance->egl_display, egl_config, instance->egl_context, flutter_context_attribs);
        if (renderer->flutter_egl_context != EGL_NO_CONTEXT) {
          flutter_config = egl_config;
        }
      }
    }

    if (renderer->flutter_egl_context == EGL_NO_CONTEXT) {
      wlr_log(WLR_ERROR, "Could not create flutter EGL context! EGL error: 0x%x", eglGetError());
    }
  } else {
    flutter_config = EGL_NO_CONFIG_KHR;
  }

  if (renderer->flutter_egl_context != EGL_NO_CONTEXT) {
    renderer->flutter_resource_egl_context = eglCreateContext(
      instance->egl_display, flutter_config, renderer->flutter_egl_context, flutter_context_attribs);
    if (renderer->flutter_resource_egl_context == EGL_NO_CONTEXT) {
      wlr_log(WLR_ERROR, "Could not create flutter resource EGL context! EGL error: 0x%x", eglGetError());
    }
  }

  renderer->current_page = 0;
  renderer->current_scene.layers_count = 0;

  if (pthread_mutex_init(&renderer->render_mutex, NULL) != 0) {
    wlr_log(WLR_ERROR, "Could not init render mutex");
  }

  eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, instance->fwr_renderer.flutter_egl_context);

  renderer->quad_rgbx_shader = make_quad_rgbx_shader(instance);
  renderer->quad_rounded_shader = make_quad_rounded_shader(instance);
  renderer->quad_external_shader = make_quad_external_shader(instance);

  fns->glGenBuffers(1, &renderer->tex_coord_buffer);
  fns->glBindBuffer(GL_ARRAY_BUFFER, renderer->tex_coord_buffer);
  fns->glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords, GL_STATIC_DRAW);

  fns->glGenBuffers(1, &renderer->quad_vert_buffer);
  fns->glBindBuffer(GL_ARRAY_BUFFER, renderer->quad_vert_buffer);
  fns->glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts, GL_STATIC_DRAW);

  instance->fl_compositor.struct_size = sizeof(FlutterCompositor);
  instance->fl_compositor.user_data = instance;
  instance->fl_compositor.avoid_backing_store_cache = true;
  instance->fl_compositor.create_backing_store_callback = create_backing_store;
  instance->fl_compositor.collect_backing_store_callback = collect_backing_store;
  instance->fl_compositor.present_layers_callback = present_layers;

  for (int i = 0; i < FWR_RENDERER_NUM_PAGES; i++) {
    struct fwr_renderer_page *page = &renderer->pages[i];
    page->instance = instance;
    wl_list_init(&page->textures);
    wl_list_init(&page->unused_textures);
  }

  eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, NULL);
}

static FlutterTransformation flutter_transform_multiply(FlutterTransformation a, FlutterTransformation b) {
  FlutterTransformation out = {
    .scaleX = a.scaleX * b.scaleX + a.skewX * b.skewY + a.transX * b.pers0,
    .skewX = a.scaleX * b.skewX + a.skewX * b.scaleY + a.transX * b.pers1,
    .transX = a.scaleX * b.transX + a.skewX * b.transY + a.transX * b.pers2,
    .skewY = a.skewY * b.scaleX + a.scaleY * b.skewY + a.transY * b.pers0,
    .scaleY = a.skewY * b.skewX + a.scaleY * b.scaleY + a.transY * b.pers1,
    .transY = a.skewY * b.transX + a.scaleY * b.transY + a.transY * b.pers2,
    .pers0 = a.pers0 * b.scaleX + a.pers1 * b.skewY + a.pers2 * b.pers0,
    .pers1 = a.pers0 * b.skewX + a.pers1 * b.scaleY + a.pers2 * b.pers1,
    .pers2 = a.pers0 * b.transX + a.pers1 * b.transY + a.pers2 * b.pers2,
  };
  return out;
}

static bool flutter_transform_is_affine(FlutterTransformation transform) {
  return transform.pers0 == 0.0 && transform.pers1 == 0.0 && transform.pers2 == 1.0;
}

static void flutter_transform_point(FlutterTransformation transform, double x, double y, double *out_x, double *out_y) {
  *out_x = x * transform.scaleX + y * transform.skewX + transform.transX;
  *out_y = x * transform.skewY + y * transform.scaleY + transform.transY;
}

static int clamp_int(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static int ellipse_inset(double dy, int rx, int ry) {
  if (rx <= 0 || ry <= 0) {
    return 0;
  }
  double ady = fabs(dy);
  if (ady >= (double)ry) {
    return rx;
  }
  double t = 1.0 - (ady * ady) / ((double)ry * (double)ry);
  if (t <= 0.0) {
    return rx;
  }
  double x_boundary = (double)rx - (double)rx * sqrt(t);
  int inset = (int)ceil(x_boundary);
  if (inset < 0) inset = 0;
  if (inset > rx) inset = rx;
  return inset;
}

static struct wlr_box flutter_transform_rect(FlutterTransformation transform,
    double left, double top, double right, double bottom) {
  // Transform a rectangle defined by float edges.
  // We floor/ceil the result to avoid "shrinking" as coordinates move fractionally.
  double tx0, ty0, tx1, ty1, tx2, ty2, tx3, ty3;
  flutter_transform_point(transform, left, top, &tx0, &ty0);
  flutter_transform_point(transform, right, top, &tx1, &ty1);
  flutter_transform_point(transform, left, bottom, &tx2, &ty2);
  flutter_transform_point(transform, right, bottom, &tx3, &ty3);

  double min_x = fmin(fmin(tx0, tx1), fmin(tx2, tx3));
  double max_x = fmax(fmax(tx0, tx1), fmax(tx2, tx3));
  double min_y = fmin(fmin(ty0, ty1), fmin(ty2, ty3));
  double max_y = fmax(fmax(ty0, ty1), fmax(ty2, ty3));

  double ix0 = floor(min_x);
  double iy0 = floor(min_y);
  double ix1 = ceil(max_x);
  double iy1 = ceil(max_y);

  return (struct wlr_box){
    .x = (int)ix0,
    .y = (int)iy0,
    .width = (int)(ix1 - ix0),
    .height = (int)(iy1 - iy0),
  };
}

static void pixman_region32_init_rounded_rect(
    pixman_region32_t *dst,
    int x, int y, int width, int height,
    int r_tl_x, int r_tl_y,
    int r_tr_x, int r_tr_y,
    int r_br_x, int r_br_y,
    int r_bl_x, int r_bl_y) {
  pixman_region32_init(dst);

  if (width <= 0 || height <= 0) {
    return;
  }

  // Clamp radii to sane values.
  int half_w = width / 2;
  int half_h = height / 2;
  r_tl_x = clamp_int(r_tl_x, 0, half_w);
  r_tr_x = clamp_int(r_tr_x, 0, half_w);
  r_br_x = clamp_int(r_br_x, 0, half_w);
  r_bl_x = clamp_int(r_bl_x, 0, half_w);
  r_tl_y = clamp_int(r_tl_y, 0, half_h);
  r_tr_y = clamp_int(r_tr_y, 0, half_h);
  r_br_y = clamp_int(r_br_y, 0, half_h);
  r_bl_y = clamp_int(r_bl_y, 0, half_h);

  int top_band = r_tl_y > r_tr_y ? r_tl_y : r_tr_y;
  int bottom_band = r_bl_y > r_br_y ? r_bl_y : r_br_y;

  // Middle band: full width.
  int middle_h = height - top_band - bottom_band;
  if (middle_h > 0) {
    pixman_region32_union_rect(dst, dst, x, y + top_band, width, middle_h);
  }

  // Top band rows.
  for (int row = 0; row < top_band; row++) {
    double y_center = (double)row + 0.5;
    int inset_left = 0;
    int inset_right = 0;

    if (r_tl_y > 0 && row < r_tl_y) {
      double dy = y_center - (double)r_tl_y;
      inset_left = ellipse_inset(dy, r_tl_x, r_tl_y);
    }
    if (r_tr_y > 0 && row < r_tr_y) {
      double dy = y_center - (double)r_tr_y;
      inset_right = ellipse_inset(dy, r_tr_x, r_tr_y);
    }

    int w = width - inset_left - inset_right;
    if (w > 0) {
      pixman_region32_union_rect(dst, dst, x + inset_left, y + row, w, 1);
    }
  }

  // Bottom band rows.
  for (int row = height - bottom_band; row < height; row++) {
    if (row < 0) continue;
    double y_center = (double)row + 0.5;
    int inset_left = 0;
    int inset_right = 0;

    if (r_bl_y > 0 && row >= height - r_bl_y) {
      double dy = y_center - (double)(height - r_bl_y);
      inset_left = ellipse_inset(dy, r_bl_x, r_bl_y);
    }
    if (r_br_y > 0 && row >= height - r_br_y) {
      double dy = y_center - (double)(height - r_br_y);
      inset_right = ellipse_inset(dy, r_br_x, r_br_y);
    }

    int w = width - inset_left - inset_right;
    if (w > 0) {
      pixman_region32_union_rect(dst, dst, x + inset_left, y + row, w, 1);
    }
  }
}

static struct wlr_box scale_box(struct wlr_box box, double scale) {
  if (scale == 1.0) {
    return box;
  }

  return (struct wlr_box){
    .x = (int)lround((double)box.x * scale),
    .y = (int)lround((double)box.y * scale),
    .width = (int)lround((double)box.width * scale),
    .height = (int)lround((double)box.height * scale),
  };
}

struct fwr_surface_render_data {
  struct wlr_render_pass *render_pass;
  FlutterTransformation transform;
  double output_scale;
  const pixman_region32_t *clip;
  const float *alpha;
  double content_scale_x;
  double content_scale_y;
};

struct fwr_rounded_clip {
  bool active;
  float rect_x;
  float rect_y;
  float rect_w;
  float rect_h;
  float radius_tl;
  float radius_tr;
  float radius_br;
  float radius_bl;
};

struct fwr_rounded_render_data {
  struct fwr_instance *instance;
  FlutterTransformation transform;
  double output_scale;
  float opacity;
  double content_scale_x;
  double content_scale_y;
  struct fwr_rounded_clip rounded_clip;
};

static void render_surface_rounded_iterator(struct wlr_surface *surface, int sx, int sy, void *data) {
  struct fwr_rounded_render_data *render_data = data;
  struct fwr_instance *instance = render_data->instance;
  struct fwr_renderer *renderer = &instance->fwr_renderer;
  struct gl_fns *fns = &renderer->fns;
  struct wlr_surface_state *surface_state = &surface->current;
  struct wlr_texture *texture = wlr_surface_get_texture(surface);
  if (texture == NULL) {
    return;
  }

  int output_width = 1;
  int output_height = 1;
  if (instance->output != NULL && instance->output->wlr_output != NULL) {
    output_width = instance->output->wlr_output->width;
    output_height = instance->output->wlr_output->height;
  }

  double left = (double)sx * render_data->content_scale_x;
  double top = (double)sy * render_data->content_scale_y;
  double right = left + (double)surface_state->width * render_data->content_scale_x;
  double bottom = top + (double)surface_state->height * render_data->content_scale_y;

  struct wlr_box dst_box = flutter_transform_rect(render_data->transform, left, top, right, bottom);
  dst_box = scale_box(dst_box, render_data->output_scale);

  float ndc_left = ((float)dst_box.x / (float)output_width) * 2.0f - 1.0f;
  float ndc_right = ((float)(dst_box.x + dst_box.width) / (float)output_width) * 2.0f - 1.0f;
  float ndc_top = 1.0f - ((float)dst_box.y / (float)output_height) * 2.0f;
  float ndc_bottom = 1.0f - ((float)(dst_box.y + dst_box.height) / (float)output_height) * 2.0f;

  const GLfloat verts[8] = {
    ndc_right, ndc_bottom,
    ndc_left, ndc_bottom,
    ndc_right, ndc_top,
    ndc_left, ndc_top,
  };

  const GLfloat texcoords[8] = {
    1.0f, 1.0f,
    0.0f, 1.0f,
    1.0f, 0.0f,
    0.0f, 0.0f,
  };

  struct quad_rounded_shader *shader = &renderer->quad_rounded_shader;
  fns->glUseProgram(shader->prog);

  fns->glEnable(GL_BLEND);
  fns->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  fns->glActiveTexture(GL_TEXTURE0);
  struct wlr_gles2_texture_attribs tex_attribs;
  wlr_gles2_texture_get_attribs(texture, &tex_attribs);
  fns->glBindTexture(tex_attribs.target, tex_attribs.tex);
  fns->glUniform1i(shader->tex, 0);

  fns->glUniform1f(shader->alpha, render_data->opacity);

  struct fwr_rounded_clip *rc = &render_data->rounded_clip;
  fns->glUniform4f(shader->clip_rect, rc->rect_x, rc->rect_y, rc->rect_w, rc->rect_h);
  fns->glUniform4f(shader->corner_radii, rc->radius_tl, rc->radius_tr, rc->radius_br, rc->radius_bl);
  fns->glUniform1f(shader->output_height, (float)output_height);

  fns->glEnableVertexAttribArray(shader->pos_attrib);
  fns->glVertexAttribPointer(shader->pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, verts);

  fns->glEnableVertexAttribArray(shader->tex_attrib);
  fns->glVertexAttribPointer(shader->tex_attrib, 2, GL_FLOAT, GL_FALSE, 0, texcoords);

  fns->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  fns->glDisableVertexAttribArray(shader->pos_attrib);
  fns->glDisableVertexAttribArray(shader->tex_attrib);
  fns->glBindTexture(GL_TEXTURE_2D, 0);
  fns->glUseProgram(0);
  fns->glDisable(GL_BLEND);
}

static void render_surface_iterator(struct wlr_surface *surface, int sx, int sy, void *data) {
  struct fwr_surface_render_data *render_data = data;
  struct wlr_surface_state *surface_state = &surface->current;
  struct wlr_texture *texture = wlr_surface_get_texture(surface);
  if (texture == NULL) {
    return;
  }

  struct wlr_fbox src_box;
  wlr_surface_get_buffer_source_box(surface, &src_box);

  // Apply optional content scaling (used to hide resize lag).
  // We do this in local surface space, then apply Flutter's transform.
  double left = (double)sx * render_data->content_scale_x;
  double top = (double)sy * render_data->content_scale_y;
  double right = left + (double)surface_state->width * render_data->content_scale_x;
  double bottom = top + (double)surface_state->height * render_data->content_scale_y;

  struct wlr_box dst_box = flutter_transform_rect(render_data->transform, left, top, right, bottom);
  dst_box = scale_box(dst_box, render_data->output_scale);

  enum wl_output_transform surface_transform = wlr_output_transform_invert(surface_state->transform);

  // Regular wlroots textured quad + optional (possibly rounded) pixman clip.
  wlr_render_pass_add_texture(render_data->render_pass, &(struct wlr_render_texture_options){
    .texture = texture,
    .src_box = src_box,
    .dst_box = dst_box,
    .alpha = render_data->alpha,
    .transform = surface_transform,
    .clip = render_data->clip,
  });
}

static void render_scene_layer_platform(struct fwr_instance *instance, struct wlr_render_pass *render_pass, struct fwr_renderer_scene_layer *layer, struct timespec *now) {
  uint32_t view_handle = layer->platform.platform_view_id;
  struct fwr_view *view;
  if (!handle_map_get(instance->views, view_handle, (void**) &view)) {
    wlr_log(WLR_ERROR, "Got invalid view handle! (%d)", view_handle);
    return;
  }

  double output_scale = 1.0;
  if (instance->output != NULL && instance->output->wlr_output != NULL) {
    output_scale = instance->output->wlr_output->scale;
  }

  // During fast interactive resize, Flutter can resize the PlatformView widget
  // before the client commits a new buffer, revealing the frame background on
  // the opposite edge. To keep things visually solid, temporarily scale the
  // last committed client buffer up to the widget size whenever the widget is
  // larger than the client buffer. When the client catches up, the scale
  // returns to 1.0 automatically.
  double content_scale_x = 1.0;
  double content_scale_y = 1.0;
  int surf_w = view->xdg_surface->surface->current.width;
  int surf_h = view->xdg_surface->surface->current.height;
  if (surf_w > 0 && surf_h > 0 && layer->size.width > 0.0 && layer->size.height > 0.0) {
    double sx = layer->size.width / (double)surf_w;
    double sy = layer->size.height / (double)surf_h;

    // Only scale up (this is what prevents "empty strip" on expansion).
    if (isfinite(sx) && sx > 1.001) {
      content_scale_x = sx;
    }
    if (isfinite(sy) && sy > 1.001) {
      content_scale_y = sy;
    }

    if (!isfinite(content_scale_x) || content_scale_x <= 0.0) {
      content_scale_x = 1.0;
    }
    if (!isfinite(content_scale_y) || content_scale_y <= 0.0) {
      content_scale_y = 1.0;
    }
  }

  float opacity = 1.0f;
  FlutterTransformation current_transform = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bool transform_affine = true;
  bool has_clip = false;
  bool has_rounded_clip = false;
  struct fwr_rounded_clip rounded_clip = {0};
  pixman_region32_t clip_region;

  for (int m = 0; m < layer->platform.mutations_count; m++) {
    FlutterPlatformViewMutation *mutation = &layer->platform.mutations[m];
    switch (mutation->type) {
      case kFlutterPlatformViewMutationTypeOpacity: {
        opacity *= mutation->opacity;
        break;
      }
      case kFlutterPlatformViewMutationTypeTransformation: {
        FlutterTransformation mutation_transform = mutation->transformation;
        if (!flutter_transform_is_affine(mutation_transform)) {
          transform_affine = false;
          break;
        }
        current_transform = flutter_transform_multiply(mutation_transform, current_transform);
        break;
      }
      case kFlutterPlatformViewMutationTypeClipRect: {
        // IMPORTANT: Don't truncate clip edges before transforming.
        // Truncation causes the effective clip to "shrink" by ~1px as the window
        // moves through fractional coordinates (visible as content getting cut off
        // towards bottom-right).
        struct wlr_box mutation_box = flutter_transform_rect(
          current_transform,
          mutation->clip_rect.left,
          mutation->clip_rect.top,
          mutation->clip_rect.right,
          mutation->clip_rect.bottom
        );
        mutation_box = scale_box(mutation_box, output_scale);

        pixman_region32_t tmp;
        pixman_region32_init_rect(&tmp, mutation_box.x, mutation_box.y, mutation_box.width, mutation_box.height);
        if (!has_clip) {
          pixman_region32_init(&clip_region);
          pixman_region32_copy(&clip_region, &tmp);
          has_clip = true;
        } else {
          pixman_region32_intersect(&clip_region, &clip_region, &tmp);
        }
        pixman_region32_fini(&tmp);
        break;
      }
      case kFlutterPlatformViewMutationTypeClipRoundedRect: {
        struct wlr_box mutation_box = flutter_transform_rect(
          current_transform,
          mutation->clip_rounded_rect.rect.left,
          mutation->clip_rounded_rect.rect.top,
          mutation->clip_rounded_rect.rect.right,
          mutation->clip_rounded_rect.rect.bottom
        );
        mutation_box = scale_box(mutation_box, output_scale);

        bool disable_radius = view->maximized || view->fullscreen;
        
        float r_tl = (float)(mutation->clip_rounded_rect.upper_left_corner_radius.width * output_scale);
        float r_tr = (float)(mutation->clip_rounded_rect.upper_right_corner_radius.width * output_scale);
        float r_br = (float)(mutation->clip_rounded_rect.lower_right_corner_radius.width * output_scale);
        float r_bl = (float)(mutation->clip_rounded_rect.lower_left_corner_radius.width * output_scale);
        
        bool has_any_radius = !disable_radius && (r_tl > 0.5f || r_tr > 0.5f || r_br > 0.5f || r_bl > 0.5f);
        
        if (has_any_radius && !has_rounded_clip) {
          has_rounded_clip = true;
          rounded_clip.active = true;
          rounded_clip.rect_x = (float)mutation_box.x;
          rounded_clip.rect_y = (float)mutation_box.y;
          rounded_clip.rect_w = (float)mutation_box.width;
          rounded_clip.rect_h = (float)mutation_box.height;
          rounded_clip.radius_tl = r_tl;
          rounded_clip.radius_tr = r_tr;
          rounded_clip.radius_br = r_br;
          rounded_clip.radius_bl = r_bl;
        }

        pixman_region32_t tmp;
        if (disable_radius || !has_any_radius) {
          pixman_region32_init_rect(&tmp, mutation_box.x, mutation_box.y, mutation_box.width, mutation_box.height);
        } else {
          int r_tl_x = (int)lround(mutation->clip_rounded_rect.upper_left_corner_radius.width * output_scale);
          int r_tl_y = (int)lround(mutation->clip_rounded_rect.upper_left_corner_radius.height * output_scale);
          int r_tr_x = (int)lround(mutation->clip_rounded_rect.upper_right_corner_radius.width * output_scale);
          int r_tr_y = (int)lround(mutation->clip_rounded_rect.upper_right_corner_radius.height * output_scale);
          int r_br_x = (int)lround(mutation->clip_rounded_rect.lower_right_corner_radius.width * output_scale);
          int r_br_y = (int)lround(mutation->clip_rounded_rect.lower_right_corner_radius.height * output_scale);
          int r_bl_x = (int)lround(mutation->clip_rounded_rect.lower_left_corner_radius.width * output_scale);
          int r_bl_y = (int)lround(mutation->clip_rounded_rect.lower_left_corner_radius.height * output_scale);

          pixman_region32_init_rounded_rect(&tmp,
            mutation_box.x, mutation_box.y, mutation_box.width, mutation_box.height,
            r_tl_x, r_tl_y,
            r_tr_x, r_tr_y,
            r_br_x, r_br_y,
            r_bl_x, r_bl_y);
        }

        if (!has_clip) {
          pixman_region32_init(&clip_region);
          pixman_region32_copy(&clip_region, &tmp);
          has_clip = true;
        } else {
          pixman_region32_intersect(&clip_region, &clip_region, &tmp);
        }
        pixman_region32_fini(&tmp);
        break;
      }
      default:
        break;
    }
  }

  if (!transform_affine) {
    wlr_log(WLR_INFO, "Non-affine Flutter transform not supported for platform view %d", view_handle);
  }

  if (has_clip) {
    const pixman_box32_t *ext = pixman_region32_extents(&clip_region);
    wlr_log(WLR_DEBUG,
      "platform view %d offset(%.2f,%.2f) size(%.2f,%.2f) output_scale %.2f transform[%.3f %.3f %.3f %.3f %.3f %.3f] clip_extents(%d,%d,%d,%d)",
      view_handle,
      (double)layer->offset.x,
      (double)layer->offset.y,
      (double)layer->size.width,
      (double)layer->size.height,
      (double)output_scale,
      (double)current_transform.scaleX,
      (double)current_transform.skewX,
      (double)current_transform.transX,
      (double)current_transform.skewY,
      (double)current_transform.scaleY,
      (double)current_transform.transY,
      ext->x1, ext->y1, (int)(ext->x2 - ext->x1), (int)(ext->y2 - ext->y1));
  } else {
    wlr_log(WLR_DEBUG,
      "platform view %d offset(%.2f,%.2f) size(%.2f,%.2f) output_scale %.2f transform[%.3f %.3f %.3f %.3f %.3f %.3f] no_clip",
      view_handle,
      (double)layer->offset.x,
      (double)layer->offset.y,
      (double)layer->size.width,
      (double)layer->size.height,
      (double)output_scale,
      (double)current_transform.scaleX,
      (double)current_transform.skewX,
      (double)current_transform.transX,
      (double)current_transform.skewY,
      (double)current_transform.scaleY,
      (double)current_transform.transY);
  }

  if (has_rounded_clip && rounded_clip.active) {
    struct fwr_rounded_render_data rounded_render_data = {
      .instance = instance,
      .transform = current_transform,
      .output_scale = output_scale,
      .opacity = opacity,
      .content_scale_x = content_scale_x,
      .content_scale_y = content_scale_y,
      .rounded_clip = rounded_clip,
    };

    wlr_surface_for_each_surface(view->xdg_surface->surface, render_surface_rounded_iterator, &rounded_render_data);
  } else {
    struct fwr_surface_render_data render_data = {
      .render_pass = render_pass,
      .transform = current_transform,
      .output_scale = output_scale,
      .clip = has_clip ? &clip_region : NULL,
      .alpha = &opacity,
      .content_scale_x = content_scale_x,
      .content_scale_y = content_scale_y,
    };

    wlr_surface_for_each_surface(view->xdg_surface->surface, render_surface_iterator, &render_data);
  }

  if (has_clip) {
    pixman_region32_fini(&clip_region);
  }

  wlr_presentation_surface_textured_on_output(view->xdg_surface->surface, instance->output->wlr_output);
  wlr_surface_send_frame_done(view->xdg_surface->surface, now);
}

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

void fwr_cached_texture_destroy(struct fwr_instance *instance,
                                struct fwr_cached_texture *cache) {
  if (cache->tex == 0 && cache->fbo == 0) {
    return;
  }

  EGLContext prev_ctx = eglGetCurrentContext();
  eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                 instance->fwr_renderer.flutter_egl_context);

  struct gl_fns *fns = &instance->fwr_renderer.fns;
  if (cache->tex != 0) {
    fns->glDeleteTextures(1, &cache->tex);
    cache->tex = 0;
  }
  if (cache->fbo != 0) {
    fns->glDeleteFramebuffers(1, &cache->fbo);
    cache->fbo = 0;
  }
  cache->width = 0;
  cache->height = 0;

  eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, prev_ctx);
}

GLuint fwr_renderer_copy_texture(struct fwr_instance *instance,
                                 GLuint texture, GLenum target,
                                 int width, int height,
                                 struct fwr_cached_texture *cache) {
  if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
    wlr_log(WLR_ERROR, "No current EGL context in copy_texture!");
    return 0;
  }

  struct fwr_renderer *renderer = &instance->fwr_renderer;
  struct gl_fns *fns = &renderer->fns;

  // Check if we need to (re)create texture
  bool need_alloc = (cache->tex == 0) ||
                    (cache->width != width) ||
                    (cache->height != height);

  if (cache->tex == 0) {
    fns->glGenTextures(1, &cache->tex);
    if (cache->tex == 0) {
      wlr_log(WLR_ERROR, "glGenTextures failed");
      return 0;
    }
    fns->glBindTexture(GL_TEXTURE_2D, cache->tex);
    fns->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    fns->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    fns->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    fns->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  } else {
    fns->glBindTexture(GL_TEXTURE_2D, cache->tex);
  }

  // Only reallocate texture storage if size changed
  if (need_alloc) {
    fns->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    cache->width = width;
    cache->height = height;
  }

  // Create FBO if needed
  if (cache->fbo == 0) {
    fns->glGenFramebuffers(1, &cache->fbo);
  }

  // Bind FBO and attach texture
  fns->glBindFramebuffer(GL_FRAMEBUFFER, cache->fbo);
  fns->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cache->tex, 0);

  GLenum status = fns->glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    wlr_log(WLR_ERROR, "Framebuffer incomplete: 0x%x, tex: %d, fbo: %d, ext: %d", status, cache->tex, cache->fbo, texture);
    fns->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
  }

  GLint viewport[4];
  fns->glGetIntegerv(GL_VIEWPORT, viewport);
  fns->glViewport(0, 0, width, height);

  // Ensure clean state for alpha preservation
  fns->glDisable(GL_BLEND);
  fns->glDisable(GL_SCISSOR_TEST);

  // Clear to fully transparent - critical for alpha shadows
  fns->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  fns->glClear(GL_COLOR_BUFFER_BIT);

  GLint old_prog;
  fns->glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);

  GLuint prog;
  GLint tex_loc, pos_loc, tex_attrib_loc;

  if (target == GL_TEXTURE_EXTERNAL_OES) {
     prog = renderer->quad_external_shader.prog;
     tex_loc = renderer->quad_external_shader.tex;
     pos_loc = renderer->quad_external_shader.pos_attrib;
     tex_attrib_loc = renderer->quad_external_shader.tex_attrib;
  } else {
     prog = renderer->quad_rgbx_shader.prog;
     tex_loc = renderer->quad_rgbx_shader.tex;
     pos_loc = renderer->quad_rgbx_shader.pos_attrib;
     tex_attrib_loc = renderer->quad_rgbx_shader.tex_attrib;
  }

  fns->glUseProgram(prog);

  fns->glActiveTexture(GL_TEXTURE0);
  fns->glBindTexture(target, texture);
  fns->glUniform1i(tex_loc, 0);

  fns->glBindBuffer(GL_ARRAY_BUFFER, renderer->quad_vert_buffer);
  fns->glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  fns->glEnableVertexAttribArray(pos_loc);

  static const GLfloat copy_texcoords[] = {
    1.0f, 0.0f,
    0.0f, 0.0f,
    1.0f, 1.0f,
    0.0f, 1.0f,
  };

  fns->glBindBuffer(GL_ARRAY_BUFFER, 0);
  fns->glVertexAttribPointer(tex_attrib_loc, 2, GL_FLOAT, GL_FALSE, 0, copy_texcoords);
  fns->glEnableVertexAttribArray(tex_attrib_loc);

  fns->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  fns->glDisableVertexAttribArray(pos_loc);
  fns->glDisableVertexAttribArray(tex_attrib_loc);

  fns->glUseProgram(old_prog);
  fns->glBindFramebuffer(GL_FRAMEBUFFER, 0);
  fns->glBindTexture(target, 0);
  fns->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

  // wlr_log(WLR_DEBUG, "Copy done. Result tex: %d", cache->tex);
  return cache->tex;
}



static void render_scene_layer_texture(struct fwr_instance *instance, struct wlr_render_pass *render_pass, struct fwr_renderer_scene_layer *layer) {
  struct fwr_renderer *renderer = &instance->fwr_renderer;

  // We are on GLES2 backend, so we can cast wlr_renderer to access internal GLES2 functions if needed
  // However, wlroots 0.18 prefers buffer imports.
  // Since we don't have easy DMA-BUF export here without more plumbing,
  // we will stick to the GL path for now but fix the coordinates.
  struct gl_fns *fns = &renderer->fns;

  int output_width = 0;
  int output_height = 0;
  if (instance->output != NULL && instance->output->wlr_output != NULL) {
    output_width = instance->output->wlr_output->width;
    output_height = instance->output->wlr_output->height;
  }

  if (output_width <= 0 || output_height <= 0) {
    output_width = 1;
    output_height = 1;
  }

  // Snap layer bounds to integer pixels to match platform-view rendering.
  // This prevents subtle frame/content desync while moving windows.
  double x0 = floor(layer->offset.x);
  double y0 = floor(layer->offset.y);
  double x1 = ceil(layer->offset.x + layer->size.width);
  double y1 = ceil(layer->offset.y + layer->size.height);

  // Normalize coordinates to -1.0 to 1.0 (NDC)
  // Y-axis: Flutter (0 top) -> GL (-1 bottom, 1 top)
  // But our previous flip was wrong. Standard GL quad: (-1,-1) bottom-left to (1,1) top-right.
  // We want (0,0) to map to top-left (-1, 1) and (w,h) to bottom-right (1, -1).

  float left = (float)((x0 / output_width) * 2.0 - 1.0);
  float right = (float)((x1 / output_width) * 2.0 - 1.0);
  // Invert Y for GL NDC (top is 1.0, bottom is -1.0)
  float top = (float)(1.0 - (y0 / output_height) * 2.0);
  float bottom = (float)(1.0 - (y1 / output_height) * 2.0);

  const GLfloat quad_verts_local[8] = {
    right, bottom, // Bottom-right
    left, bottom,  // Bottom-left
    right, top,    // Top-right
    left, top,     // Top-left
  };

  fns->glUseProgram(renderer->quad_rgbx_shader.prog);

  fns->glEnableVertexAttribArray(renderer->quad_rgbx_shader.pos_attrib);
  fns->glBindBuffer(GL_ARRAY_BUFFER, renderer->quad_vert_buffer);
  fns->glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts_local), quad_verts_local, GL_STREAM_DRAW);
  fns->glVertexAttribPointer(renderer->quad_rgbx_shader.pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, (void*) 0);

  fns->glEnableVertexAttribArray(renderer->quad_rgbx_shader.tex_attrib);
  fns->glBindBuffer(GL_ARRAY_BUFFER, renderer->tex_coord_buffer);
  fns->glVertexAttribPointer(renderer->quad_rgbx_shader.tex_attrib, 2, GL_FLOAT, GL_FALSE, 0, (void*) 0);

  fns->glActiveTexture(GL_TEXTURE0);

  fns->glBindTexture(GL_TEXTURE_2D, layer->texture.texture->texture);
  fns->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  fns->glDisableVertexAttribArray(renderer->quad_rgbx_shader.pos_attrib);
  fns->glDisableVertexAttribArray(renderer->quad_rgbx_shader.tex_attrib);

  fns->glBindTexture(GL_TEXTURE_2D, 0);
  fns->glBindBuffer(GL_ARRAY_BUFFER, 0);

  fns->glUseProgram(0);
}

void fwr_renderer_render_scene(struct fwr_instance *instance, struct wlr_render_pass *render_pass) {
  struct fwr_renderer *renderer = &instance->fwr_renderer;

  struct timespec now;
 	clock_gettime(CLOCK_MONOTONIC, &now);

  pthread_mutex_lock(&renderer->render_mutex);

  if (renderer->current_scene.sync != 0) {
      eglWaitSync(instance->egl_display, renderer->current_scene.sync, 0);
      eglDestroySync(instance->egl_display, renderer->current_scene.sync);
      renderer->current_scene.sync = 0;
  }

  // Per-frame layer logging disabled - too verbose
  // static int log_counter = 0;
  // bool should_log = (log_counter++ % 300 == 0);

  for (int i = 0; i < renderer->current_scene.layers_count; i++) {
    struct fwr_renderer_scene_layer *layer = &renderer->current_scene.layers[i];
    switch (layer->type) {
    case sceneLayerPlatform:
      render_scene_layer_platform(instance, render_pass, layer, &now);
      break;
    case sceneLayerTexture:
      render_scene_layer_texture(instance, render_pass, layer);
      break;
    }
  }

  pthread_mutex_unlock(&renderer->render_mutex);
}

void fwr_renderer_update_scene_positions(struct fwr_instance *instance) {
  if (instance->scene == NULL) {
    return;
  }

  // Scene node enable/disable is managed by map/unmap handlers.
  // Here we just sync positions with Flutter's view positions.
  // Views rendered via external textures won't have platform layers,
  // so we update all mapped views directly.

  struct fwr_view *view;
  wl_list_for_each(view, &instance->views_list, link) {
    if (view->scene_tree == NULL) {
      continue;
    }
    // Position is set by surface_set_position platform channel message
    // Note: wlr_scene_xdg_surface handles geometry offset internally
    int titlebar_offset = view->uses_ssd ? 38 : 0;
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y + titlebar_offset);
  }
}
