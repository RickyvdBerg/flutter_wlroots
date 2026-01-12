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
#define DRM_FORMAT_ARGB8888 0x34325241
#define DRM_FORMAT_ABGR8888 0x34324241

static void cpu_buffer_destroy(struct wlr_buffer *wlr_buffer) {
  struct fwr_cpu_buffer *buf = wl_container_of(wlr_buffer, buf, base);
  free(buf->data);
  free(buf);
}

static bool cpu_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
    uint32_t flags, void **data, uint32_t *format, size_t *stride) {
  struct fwr_cpu_buffer *buf = wl_container_of(wlr_buffer, buf, base);
  *data = buf->data;
  *format = buf->format;
  *stride = buf->stride;
  return true;
}

static void cpu_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
}

static const struct wlr_buffer_impl cpu_buffer_impl = {
  .destroy = cpu_buffer_destroy,
  .begin_data_ptr_access = cpu_buffer_begin_data_ptr_access,
  .end_data_ptr_access = cpu_buffer_end_data_ptr_access,
};

static struct fwr_cpu_buffer *cpu_buffer_create(int width, int height, uint32_t format) {
  struct fwr_cpu_buffer *buf = calloc(1, sizeof(struct fwr_cpu_buffer));
  if (buf == NULL) {
    return NULL;
  }

  buf->stride = (size_t)width * 4;
  buf->format = format;
  buf->data = malloc(buf->stride * (size_t)height);
  if (buf->data == NULL) {
    free(buf);
    return NULL;
  }

  wlr_buffer_init(&buf->base, &cpu_buffer_impl, width, height);
  return buf;
}

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

    renderer->flutter_readback_egl_context = eglCreateContext(
      instance->egl_display, flutter_config, renderer->flutter_egl_context, flutter_context_attribs);
    if (renderer->flutter_readback_egl_context == EGL_NO_CONTEXT) {
      wlr_log(WLR_ERROR, "Could not create flutter readback EGL context! EGL error: 0x%x", eglGetError());
    }
  }

  renderer->current_page = 0;
  renderer->current_scene.layers_count = 0;
  renderer->flutter_scene_buffers = NULL;
  renderer->flutter_scene_buffers_len = 0;
  renderer->flutter_scene_buffers_cap = 0;

  if (pthread_mutex_init(&renderer->render_mutex, NULL) != 0) {
    wlr_log(WLR_ERROR, "Could not init render mutex");
  }

  eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, instance->fwr_renderer.flutter_egl_context);

  renderer->quad_rgbx_shader = make_quad_rgbx_shader(instance);

  const GLfloat x1 = 0.0;
  const GLfloat x2 = 1.0;
  const GLfloat y1 = 0.0;
  const GLfloat y2 = 1.0;
  const GLfloat texcoords[] = {
    x2, y2,
    x1, y2,
    x2, y1,
    x1, y1
  };

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

static struct wlr_box flutter_transform_box(FlutterTransformation transform, struct wlr_box box) {
  double x0 = box.x;
  double y0 = box.y;
  double x1 = box.x + box.width;
  double y1 = box.y + box.height;

  double tx0, ty0, tx1, ty1, tx2, ty2, tx3, ty3;
  flutter_transform_point(transform, x0, y0, &tx0, &ty0);
  flutter_transform_point(transform, x1, y0, &tx1, &ty1);
  flutter_transform_point(transform, x0, y1, &tx2, &ty2);
  flutter_transform_point(transform, x1, y1, &tx3, &ty3);

  double min_x = fmin(fmin(tx0, tx1), fmin(tx2, tx3));
  double max_x = fmax(fmax(tx0, tx1), fmax(tx2, tx3));
  double min_y = fmin(fmin(ty0, ty1), fmin(ty2, ty3));
  double max_y = fmax(fmax(ty0, ty1), fmax(ty2, ty3));

  return (struct wlr_box){
    .x = (int)min_x,
    .y = (int)min_y,
    .width = (int)(max_x - min_x),
    .height = (int)(max_y - min_y),
  };
}

static struct wlr_box intersect_boxes(struct wlr_box a, struct wlr_box b) {
  int left = a.x > b.x ? a.x : b.x;
  int top = a.y > b.y ? a.y : b.y;
  int right = (a.x + a.width) < (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
  int bottom = (a.y + a.height) < (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);

  if (right < left || bottom < top) {
    return (struct wlr_box){ .x = left, .y = top, .width = 0, .height = 0 };
  }

  return (struct wlr_box){
    .x = left,
    .y = top,
    .width = right - left,
    .height = bottom - top,
  };
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
};

static void render_surface_iterator(struct wlr_surface *surface, int sx, int sy, void *data) {
  struct fwr_surface_render_data *render_data = data;
  struct wlr_surface_state *surface_state = &surface->current;
  struct wlr_texture *texture = wlr_surface_get_texture(surface);
  if (texture == NULL) {
    return;
  }

  struct wlr_fbox src_box;
  wlr_surface_get_buffer_source_box(surface, &src_box);

  struct wlr_box dst_box = {
    .x = sx,
    .y = sy,
    .width = surface_state->width,
    .height = surface_state->height,
  };

  dst_box = flutter_transform_box(render_data->transform, dst_box);
  dst_box = scale_box(dst_box, render_data->output_scale);

  enum wl_output_transform surface_transform = wlr_output_transform_invert(surface_state->transform);

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

  float opacity = 1.0f;
  FlutterTransformation current_transform = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bool transform_affine = true;
  bool has_clip = false;
  pixman_region32_t clip_region;
  struct wlr_box clip_box = {0};

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
        struct wlr_box mutation_box = {
          .x = mutation->clip_rect.left,
          .y = mutation->clip_rect.top,
          .width = mutation->clip_rect.right - mutation->clip_rect.left,
          .height = mutation->clip_rect.bottom - mutation->clip_rect.top,
        };
        if (transform_affine) {
          mutation_box = flutter_transform_box(current_transform, mutation_box);
          if (has_clip) {
            clip_box = intersect_boxes(clip_box, mutation_box);
          } else {
            clip_box = mutation_box;
            has_clip = true;
          }
        }
        break;
      }
      case kFlutterPlatformViewMutationTypeClipRoundedRect: {
        struct wlr_box mutation_box = {
          .x = mutation->clip_rounded_rect.rect.left,
          .y = mutation->clip_rounded_rect.rect.top,
          .width = mutation->clip_rounded_rect.rect.right - mutation->clip_rounded_rect.rect.left,
          .height = mutation->clip_rounded_rect.rect.bottom - mutation->clip_rounded_rect.rect.top,
        };
        if (transform_affine) {
          mutation_box = flutter_transform_box(current_transform, mutation_box);
          if (has_clip) {
            clip_box = intersect_boxes(clip_box, mutation_box);
          } else {
            clip_box = mutation_box;
            has_clip = true;
          }
        }
        break;
      }
      default:
        break;
    }
  }

  if (!transform_affine) {
    wlr_log(WLR_INFO, "Non-affine Flutter transform not supported for platform view %d", view_handle);
  } else if (has_clip) {
    clip_box = flutter_transform_box(current_transform, clip_box);
  }

  wlr_log(WLR_DEBUG,
    "platform view %d offset(%.2f,%.2f) size(%.2f,%.2f) output_scale %.2f transform[%.3f %.3f %.3f %.3f %.3f %.3f]",
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

  if (has_clip) {
    clip_box = scale_box(clip_box, output_scale);
    pixman_region32_init_rect(&clip_region, clip_box.x, clip_box.y, clip_box.width, clip_box.height);
  }

  struct fwr_surface_render_data render_data = {
    .render_pass = render_pass,
    .transform = current_transform,
    .output_scale = output_scale,
    .clip = has_clip ? &clip_region : NULL,
    .alpha = &opacity,
  };

  wlr_surface_for_each_surface(view->xdg_surface->surface, render_surface_iterator, &render_data);

  if (has_clip) {
    pixman_region32_fini(&clip_region);
  }

  wlr_presentation_surface_textured_on_output(view->xdg_surface->surface, instance->output->wlr_output);
  wlr_surface_send_frame_done(view->xdg_surface->surface, now);
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

  // Normalize coordinates to -1.0 to 1.0 (NDC)
  // Y-axis: Flutter (0 top) -> GL (-1 bottom, 1 top)
  // But our previous flip was wrong. Standard GL quad: (-1,-1) bottom-left to (1,1) top-right.
  // We want (0,0) to map to top-left (-1, 1) and (w,h) to bottom-right (1, -1).

  float left = (float)((layer->offset.x / output_width) * 2.0 - 1.0);
  float right = (float)(((layer->offset.x + layer->size.width) / output_width) * 2.0 - 1.0);
  // Invert Y for GL NDC (top is 1.0, bottom is -1.0)
  float top = (float)(1.0 - (layer->offset.y / output_height) * 2.0);
  float bottom = (float)(1.0 - ((layer->offset.y + layer->size.height) / output_height) * 2.0);

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

static size_t count_texture_layers(const struct fwr_renderer_scene *scene) {
  size_t count = 0;
  for (size_t i = 0; i < scene->layers_count; i++) {
    if (scene->layers[i].type == sceneLayerTexture && scene->layers[i].texture.texture != NULL) {
      count++;
    }
  }
  return count;
}

static bool ensure_flutter_scene_buffers(struct fwr_instance *instance, struct fwr_renderer *renderer, size_t needed) {
  if (needed > renderer->flutter_scene_buffers_cap) {
    size_t new_cap = renderer->flutter_scene_buffers_cap == 0 ? 4 : renderer->flutter_scene_buffers_cap;
    while (new_cap < needed) {
      new_cap *= 2;
    }

    struct fwr_flutter_scene_buffer *new_arr = realloc(renderer->flutter_scene_buffers, new_cap * sizeof(*new_arr));
    if (new_arr == NULL) {
      return false;
    }

    for (size_t i = renderer->flutter_scene_buffers_cap; i < new_cap; i++) {
      new_arr[i].scene_buffer = NULL;
      new_arr[i].last_cpu_buffer = NULL;
    }

    renderer->flutter_scene_buffers = new_arr;
    renderer->flutter_scene_buffers_cap = new_cap;
  }

  for (size_t i = renderer->flutter_scene_buffers_len; i < needed; i++) {
    renderer->flutter_scene_buffers[i].scene_buffer = wlr_scene_buffer_create(&instance->scene->tree, NULL);
    renderer->flutter_scene_buffers[i].scene_buffer->node.data = instance;
    renderer->flutter_scene_buffers[i].last_cpu_buffer = NULL;
  }

  renderer->flutter_scene_buffers_len = needed;
  return true;
}

static void trim_flutter_scene_buffers(struct fwr_renderer *renderer, size_t needed) {
  for (size_t i = needed; i < renderer->flutter_scene_buffers_len; i++) {
    if (renderer->flutter_scene_buffers[i].scene_buffer != NULL) {
      wlr_scene_node_destroy(&renderer->flutter_scene_buffers[i].scene_buffer->node);
      renderer->flutter_scene_buffers[i].scene_buffer = NULL;
    }
    if (renderer->flutter_scene_buffers[i].last_cpu_buffer != NULL) {
      wlr_buffer_drop(&renderer->flutter_scene_buffers[i].last_cpu_buffer->base);
      renderer->flutter_scene_buffers[i].last_cpu_buffer = NULL;
    }
  }

  renderer->flutter_scene_buffers_len = needed;
}

static void swizzle_rgba_to_bgra(void *data, size_t stride, int height) {
  uint8_t *bytes = data;
  size_t width = stride / 4;

  for (int y = 0; y < height; y++) {
    uint8_t *row = bytes + (size_t)y * stride;
    for (size_t x = 0; x < width; x++) {
      uint8_t *px = row + x * 4;
      uint8_t tmp = px[0];
      px[0] = px[2];
      px[2] = tmp;
    }
  }
}

static void flip_buffer_y(void *data, size_t stride, int height) {
  if (height <= 1) {
    return;
  }

  uint8_t *bytes = data;
  uint8_t *tmp = malloc(stride);
  if (tmp == NULL) {
    return;
  }

  for (int y = 0; y < height / 2; y++) {
    uint8_t *top = bytes + (size_t)y * stride;
    uint8_t *bottom = bytes + (size_t)(height - 1 - y) * stride;
    memcpy(tmp, top, stride);
    memcpy(top, bottom, stride);
    memcpy(bottom, tmp, stride);
  }

  free(tmp);
}

void fwr_renderer_update_scene_buffer(struct fwr_instance *instance) {
  struct fwr_renderer *renderer = &instance->fwr_renderer;
  struct gl_fns *fns = &renderer->fns;

  if (instance->scene == NULL) {
    return;
  }

  pthread_mutex_lock(&renderer->render_mutex);

  if (!renderer->current_scene.needs_update) {
    pthread_mutex_unlock(&renderer->render_mutex);
    return;
  }

  size_t texture_layers = count_texture_layers(&renderer->current_scene);

  if (texture_layers < renderer->flutter_scene_buffers_len) {
    trim_flutter_scene_buffers(renderer, texture_layers);
  }

  if (!ensure_flutter_scene_buffers(instance, renderer, texture_layers)) {
    wlr_log(WLR_ERROR, "Failed to allocate Flutter scene buffer array");
    renderer->current_scene.needs_update = false;
    pthread_mutex_unlock(&renderer->render_mutex);
    return;
  }

  EGLDisplay prev_display = eglGetCurrentDisplay();
  EGLContext prev_context = eglGetCurrentContext();
  EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
  EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);

  EGLContext readback_context = renderer->flutter_readback_egl_context != EGL_NO_CONTEXT ? renderer->flutter_readback_egl_context : renderer->flutter_egl_context;
  if (!eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, readback_context)) {
    wlr_log(WLR_ERROR, "Failed to make Flutter context current for readback (egl error 0x%x)", eglGetError());
    renderer->current_scene.needs_update = false;
    pthread_mutex_unlock(&renderer->render_mutex);
    return;
  }

  if (renderer->current_scene.sync != 0) {
    eglWaitSync(instance->egl_display, renderer->current_scene.sync, 0);
    eglDestroySync(instance->egl_display, renderer->current_scene.sync);
    renderer->current_scene.sync = 0;
  }

  if (renderer->readback_fbo == 0) {
    fns->glGenFramebuffers(1, &renderer->readback_fbo);
  }

  const uint32_t cpu_drm_format = DRM_FORMAT_ARGB8888;

  size_t texture_index = 0;
  for (size_t i = 0; i < renderer->current_scene.layers_count; i++) {
    struct fwr_renderer_scene_layer *layer = &renderer->current_scene.layers[i];
    if (layer->type != sceneLayerTexture || layer->texture.texture == NULL) {
      continue;
    }

    struct fwr_renderer_page_texture *tex = layer->texture.texture;
    int width = (int)tex->width;
    int height = (int)tex->height;
    if (width <= 0 || height <= 0) {
      texture_index++;
      continue;
    }

    struct fwr_cpu_buffer *new_cpu_buffer = cpu_buffer_create(width, height, cpu_drm_format);
    if (new_cpu_buffer == NULL) {
      wlr_log(WLR_ERROR, "Failed to allocate CPU buffer for Flutter readback (%dx%d)", width, height);
      texture_index++;
      continue;
    }

    fns->glBindFramebuffer(GL_FRAMEBUFFER, renderer->readback_fbo);
    fns->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->texture, 0);

    GLenum status = fns->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      wlr_log(WLR_ERROR, "Flutter readback FBO incomplete: 0x%x (tex=%u %dx%d)", status, tex->texture, width, height);
      wlr_buffer_drop(&new_cpu_buffer->base);
      texture_index++;
      continue;
    }

    fns->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, new_cpu_buffer->data);
    GLenum err = fns->glGetError();
    if (err != GL_NO_ERROR) {
      wlr_log(WLR_ERROR, "glReadPixels failed: 0x%x", err);
    }
    swizzle_rgba_to_bgra(new_cpu_buffer->data, new_cpu_buffer->stride, height);
    flip_buffer_y(new_cpu_buffer->data, new_cpu_buffer->stride, height);

    struct wlr_scene_buffer *scene_buffer = renderer->flutter_scene_buffers[texture_index].scene_buffer;
    int x = (int)lround((double)layer->offset.x);
    int y = (int)lround((double)layer->offset.y);
    wlr_scene_node_set_position(&scene_buffer->node, x, y);
    wlr_scene_buffer_set_buffer(scene_buffer, &new_cpu_buffer->base);

    if (renderer->flutter_scene_buffers[texture_index].last_cpu_buffer != NULL) {
      wlr_buffer_drop(&renderer->flutter_scene_buffers[texture_index].last_cpu_buffer->base);
    }
    renderer->flutter_scene_buffers[texture_index].last_cpu_buffer = new_cpu_buffer;

    texture_index++;
  }

  fns->glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (prev_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(prev_display, prev_draw, prev_read, prev_context);
  } else {
    eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }

  struct fwr_view *list_view;
  wl_list_for_each(list_view, &instance->views_list, link) {
    if (list_view->scene_tree != NULL) {
      wlr_scene_node_set_enabled(&list_view->scene_tree->node, false);
    }
  }

  texture_index = 0;
  for (size_t i = 0; i < renderer->current_scene.layers_count; i++) {
    struct fwr_renderer_scene_layer *layer = &renderer->current_scene.layers[i];

    if (layer->type == sceneLayerTexture && layer->texture.texture != NULL) {
      wlr_scene_node_raise_to_top(&renderer->flutter_scene_buffers[texture_index].scene_buffer->node);
      texture_index++;
      continue;
    }

    if (layer->type != sceneLayerPlatform) {
      continue;
    }

    uint32_t view_handle = (uint32_t)layer->platform.platform_view_id;
    struct fwr_view *view;
    if (!handle_map_get(instance->views, view_handle, (void**)&view)) {
      continue;
    }
    if (view == NULL || view->scene_tree == NULL) {
      continue;
    }

    FlutterTransformation current_transform = (FlutterTransformation){1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool transform_affine = true;

    for (size_t m = 0; m < layer->platform.mutations_count; m++) {
      const FlutterPlatformViewMutation *mutation = &layer->platform.mutations[m];
      if (mutation->type != kFlutterPlatformViewMutationTypeTransformation) {
        continue;
      }

      FlutterTransformation mutation_transform = mutation->transformation;
      if (!flutter_transform_is_affine(mutation_transform)) {
        transform_affine = false;
        break;
      }

      current_transform = flutter_transform_multiply(mutation_transform, current_transform);
    }

    if (!transform_affine || current_transform.scaleX != 1.0 || current_transform.scaleY != 1.0 ||
        current_transform.skewX != 0.0 || current_transform.skewY != 0.0) {
      wlr_log(WLR_DEBUG, "Platform view %u has non-translation transform; ignoring scale/skew", view_handle);
    }

    double x = (double)layer->offset.x;
    double y = (double)layer->offset.y;

    if (transform_affine && current_transform.scaleX == 1.0 && current_transform.scaleY == 1.0 &&
        current_transform.skewX == 0.0 && current_transform.skewY == 0.0 &&
        layer->offset.x == 0.0 && layer->offset.y == 0.0 &&
        (current_transform.transX != 0.0 || current_transform.transY != 0.0)) {
      x += current_transform.transX;
      y += current_transform.transY;
    }

    view->x = (int)lround(x);
    view->y = (int)lround(y);
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
  }

  renderer->current_scene.needs_update = false;
  pthread_mutex_unlock(&renderer->render_mutex);
}

