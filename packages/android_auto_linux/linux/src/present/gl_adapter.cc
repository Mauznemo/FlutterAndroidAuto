// SPDX-License-Identifier: GPL-3.0-or-later
#include "gl_adapter.h"

#include <drm/drm_fourcc.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <flutter_linux/flutter_linux.h>

#include <atomic>
#include <cstring>
#include <string>

#include "../frame_ring.h"
#include "texture_registry.h"

namespace {

// Whether the raster thread's context can import a dmabuf. Optimistic until the first
// populate has had a chance to look, because the decoder has to choose a backend before
// Flutter has necessarily drawn anything.
std::atomic<bool> g_dmabuf_supported{true};
std::atomic<bool> g_dmabuf_probed{false};

// YUV to RGB, as three columns because that is the order glUniformMatrix3fv wants.
// Both variants include the 255/219 luma scale for limited range material; the full
// range ones do not, since there is nothing to expand.
const float kBt601Limited[9] = {1.164383f, 1.164383f, 1.164383f, 0.0f,     -0.391762f,
                                2.017232f, 1.596027f, -0.812968f, 0.0f};
const float kBt709Limited[9] = {1.164383f, 1.164383f, 1.164383f, 0.0f,     -0.213249f,
                                2.112402f, 1.792741f, -0.532909f, 0.0f};
const float kBt601Full[9] = {1.0f,   1.0f,       1.0f,   0.0f, -0.344136f,
                             1.772f, 1.402f,     -0.714136f, 0.0f};
const float kBt709Full[9] = {1.0f,     1.0f,       1.0f,   0.0f, -0.187324f,
                             1.8556f,  1.5748f,    -0.468124f, 0.0f};

const char kVertexBody[] = R"(
out vec2 v_uv;
void main() {
  // A single triangle that covers the viewport, built from the vertex index so there
  // is no buffer to bind. Its third corner falls outside the screen and is clipped.
  vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_uv = corner;
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char kFragmentBody[] = R"(
uniform sampler2D u_luma;
uniform sampler2D u_chroma;
uniform mat3 u_matrix;
uniform vec3 u_offset;
in vec2 v_uv;
out vec4 frag_color;
void main() {
  // The chroma texture is half size in both directions, so sampling it with the same
  // coordinates and GL_LINEAR is the bilinear upsample, for free.
  float luma = texture(u_luma, v_uv).r;
  vec2 chroma = texture(u_chroma, v_uv).rg;
  vec3 yuv = vec3(luma, chroma.x, chroma.y) - u_offset;
  frag_color = vec4(clamp(u_matrix * yuv, 0.0, 1.0), 1.0);
}
)";

// GLSL 1.50 and GLSL ES 3.00 are the same language for a shader this simple, they just
// disagree about the version line and whether precision has to be spelled out.
std::string ShaderSource(const char* body, bool fragment) {
  std::string source;
  if (epoxy_is_desktop_gl()) {
    source = "#version 150\n";
  } else {
    source = "#version 300 es\n";
    if (fragment) {
      // Chroma at mediump shows up as banding on gradients, and this shader is three
      // instructions, so there is nothing to save by being stingy.
      source += "precision highp float;\n";
    }
  }
  source += body;
  return source;
}

bool HasModernShaders() {
  const int version = epoxy_gl_version();
  return epoxy_is_desktop_gl() ? version >= 32 : version >= 30;
}

GLuint CompileShader(GLenum type, const std::string& source, std::string* error) {
  const GLuint shader = glCreateShader(type);
  const char* text = source.c_str();
  glShaderSource(shader, 1, &text, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_FALSE) {
    char log[512] = {0};
    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    *error = log;
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

// The GL state populate() is allowed to disturb, saved on the way in and put back on
// the way out.
//
// Flutter calls populate() from inside its own rasterisation, so anything left changed
// here lands in the middle of Impeller's frame. Impeller re-binds most of what it uses,
// but "most" is not a contract, and a texture that renders correctly while corrupting
// the widgets around it is a miserable thing to debug.
struct GlStateGuard {
  GLint framebuffer = 0;
  GLint viewport[4] = {0, 0, 0, 0};
  GLint program = 0;
  GLint active_texture = GL_TEXTURE0;
  GLint texture_unit0 = 0;
  GLint texture_unit1 = 0;
  GLint texture_unit2 = 0;
  GLint vertex_array = 0;
  GLboolean scissor = GL_FALSE;
  GLboolean depth = GL_FALSE;
  GLboolean stencil = GL_FALSE;
  GLboolean blend = GL_FALSE;
  GLboolean cull = GL_FALSE;
  GLboolean color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  GLboolean depth_mask = GL_TRUE;

  GlStateGuard() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_unit0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_unit1);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_unit2);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    depth = glIsEnabled(GL_DEPTH_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    blend = glIsEnabled(GL_BLEND);
    cull = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
  }

  ~GlStateGuard() {
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_unit2));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_unit1));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_unit0));
    glActiveTexture(static_cast<GLenum>(active_texture));
    glBindVertexArray(static_cast<GLuint>(vertex_array));
    glUseProgram(static_cast<GLuint>(program));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    Restore(GL_SCISSOR_TEST, scissor);
    Restore(GL_DEPTH_TEST, depth);
    Restore(GL_STENCIL_TEST, stencil);
    Restore(GL_BLEND, blend);
    Restore(GL_CULL_FACE, cull);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glDepthMask(depth_mask);
  }

  static void Restore(GLenum capability, GLboolean enabled) {
    if (enabled == GL_TRUE) {
      glEnable(capability);
    } else {
      glDisable(capability);
    }
  }
};

}  // namespace

// An FlTextureGL fed from the FrameRing.
//
// Flutter calls populate() on its raster thread with its own GL context already
// current, which is the one place in this project where GL calls are legal. Two paths
// land here:
//
//   kCpuRgba   uploaded straight into the output texture, no shader involved
//   kDmabuf    each layer imported as an EGLImage, then converted to RGBA by a shader
//              into the same output texture, because Flutter only takes GL_RGBA8 and
//              the decoder produces NV12
//
// The import happens here rather than on the decoder thread on purpose: this is the one
// thread with a context, so there is no second context to share and no fence to get
// right. The cost is one eglCreateImageKHR pair per frame, which is a few microseconds.
struct _AaVideoTexture {
  FlTextureGL parent_instance;
  aa::FrameRing* ring;
  GLuint name;
  // Dimensions the output texture was last allocated at. A change means glTexImage2D
  // instead of the cheaper glTexSubImage2D.
  int32_t allocated_width;
  int32_t allocated_height;

  // The dmabuf path. All of it is created lazily on the first dmabuf frame, so a
  // session that only ever shows the test pattern never compiles a shader.
  gboolean converter_ready;
  gboolean converter_failed;
  GLuint program;
  GLuint framebuffer;
  GLuint vertex_array;
  GLuint plane_textures[2];
  GLint uniform_luma;
  GLint uniform_chroma;
  GLint uniform_matrix;
  GLint uniform_offset;
};

G_DECLARE_FINAL_TYPE(AaVideoTexture, aa_video_texture, AA, VIDEO_TEXTURE, FlTextureGL)
G_DEFINE_TYPE(AaVideoTexture, aa_video_texture, fl_texture_gl_get_type())

// Records once, from the raster thread, whether this context can do the dmabuf path.
// Everything the zero copy route needs has to be present: EGL, the import extension,
// the GL side of EGLImage, and a shading language new enough for the converter.
static void aa_video_texture_probe(AaVideoTexture* self) {
  if (g_dmabuf_probed.load()) {
    return;
  }

  bool supported = true;
  const EGLDisplay display = eglGetCurrentDisplay();
  if (display == EGL_NO_DISPLAY) {
    // A GLX context, which happens on X11. Nothing to import a dmabuf into.
    supported = false;
  } else if (!epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import")) {
    supported = false;
  } else if (!epoxy_has_gl_extension("GL_OES_EGL_image") &&
             !epoxy_has_gl_extension("GL_EXT_EGL_image_storage")) {
    supported = false;
  } else if (!HasModernShaders()) {
    supported = false;
  }

  g_dmabuf_supported.store(supported);
  g_dmabuf_probed.store(true);
}

// Builds the NV12 to RGBA converter. Raster thread only.
static gboolean aa_video_texture_ensure_converter(AaVideoTexture* self) {
  if (self->converter_ready) {
    return TRUE;
  }
  if (self->converter_failed) {
    return FALSE;
  }
  self->converter_failed = TRUE;

  // Guarded here as well as in the probe, because the decoder may already have a frame
  // in flight when the probe turns the dmabuf path off. Below GLES 3.0 there is no
  // vertex array object and epoxy aborts the process rather than returning an error
  // when an entry point is missing.
  if (!HasModernShaders()) {
    return FALSE;
  }

  std::string error;
  const GLuint vertex =
      CompileShader(GL_VERTEX_SHADER, ShaderSource(kVertexBody, false), &error);
  if (vertex == 0) {
    g_warning("android_auto: the video vertex shader did not compile: %s", error.c_str());
    return FALSE;
  }
  const GLuint fragment =
      CompileShader(GL_FRAGMENT_SHADER, ShaderSource(kFragmentBody, true), &error);
  if (fragment == 0) {
    glDeleteShader(vertex);
    g_warning("android_auto: the video fragment shader did not compile: %s",
              error.c_str());
    return FALSE;
  }

  self->program = glCreateProgram();
  glAttachShader(self->program, vertex);
  glAttachShader(self->program, fragment);
  glLinkProgram(self->program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);

  GLint linked = GL_FALSE;
  glGetProgramiv(self->program, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE) {
    char log[512] = {0};
    glGetProgramInfoLog(self->program, sizeof(log) - 1, nullptr, log);
    g_warning("android_auto: the video shader did not link: %s", log);
    glDeleteProgram(self->program);
    self->program = 0;
    return FALSE;
  }

  self->uniform_luma = glGetUniformLocation(self->program, "u_luma");
  self->uniform_chroma = glGetUniformLocation(self->program, "u_chroma");
  self->uniform_matrix = glGetUniformLocation(self->program, "u_matrix");
  self->uniform_offset = glGetUniformLocation(self->program, "u_offset");

  glGenFramebuffers(1, &self->framebuffer);
  // Core profile refuses to draw without one, and the shader reads no attributes, so an
  // empty one is all that is needed.
  glGenVertexArrays(1, &self->vertex_array);
  glGenTextures(2, self->plane_textures);
  for (int i = 0; i < 2; ++i) {
    glBindTexture(GL_TEXTURE_2D, self->plane_textures[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  self->converter_failed = FALSE;
  self->converter_ready = TRUE;
  return TRUE;
}

// Imports one dmabuf layer into `texture`. Returns the EGLImage, which the caller
// destroys once the draw that samples it has been issued.
static EGLImageKHR aa_video_texture_import_layer(EGLDisplay display,
                                                 const aa::FrameLayer& layer,
                                                 GLuint texture) {
  EGLint attributes[17];
  int count = 0;
  attributes[count++] = EGL_WIDTH;
  attributes[count++] = layer.width;
  attributes[count++] = EGL_HEIGHT;
  attributes[count++] = layer.height;
  attributes[count++] = EGL_LINUX_DRM_FOURCC_EXT;
  attributes[count++] = static_cast<EGLint>(layer.fourcc);
  attributes[count++] = EGL_DMA_BUF_PLANE0_FD_EXT;
  attributes[count++] = layer.fd;
  attributes[count++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
  attributes[count++] = static_cast<EGLint>(layer.offset);
  attributes[count++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
  attributes[count++] = static_cast<EGLint>(layer.pitch);
  // The modifier says how the driver tiled the buffer. Passing it needs a second
  // extension; without it the import only works for a linear buffer, which a decoder
  // never produces, so a driver missing this ends up on the software path.
  if (layer.modifier != DRM_FORMAT_MOD_INVALID &&
      epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import_modifiers")) {
    attributes[count++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attributes[count++] = static_cast<EGLint>(layer.modifier & 0xffffffffu);
    attributes[count++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attributes[count++] = static_cast<EGLint>(layer.modifier >> 32);
  }
  attributes[count++] = EGL_NONE;

  const EGLImageKHR image = eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                              EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
  if (image == EGL_NO_IMAGE_KHR) {
    return EGL_NO_IMAGE_KHR;
  }
  glBindTexture(GL_TEXTURE_2D, texture);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(image));
  return image;
}

// Makes sure the output texture exists and is `width` by `height` RGBA8.
static void aa_video_texture_resize_output(AaVideoTexture* self, int32_t width,
                                           int32_t height, const void* pixels) {
  if (self->name == 0) {
    glGenTextures(1, &self->name);
    glBindTexture(GL_TEXTURE_2D, self->name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glBindTexture(GL_TEXTURE_2D, self->name);
  }
  if (width != self->allocated_width || height != self->allocated_height) {
    // A resolution change mid session lands here. The texture is reallocated but not
    // replaced, so the texture id Flutter is holding stays valid and the widget tree
    // does not have to be rebuilt.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
    self->allocated_width = width;
    self->allocated_height = height;
  } else if (pixels != nullptr) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                    pixels);
  }
}

static gboolean aa_video_texture_draw_dmabuf(AaVideoTexture* self,
                                             const aa::Frame& frame) {
  const EGLDisplay display = eglGetCurrentDisplay();
  if (display == EGL_NO_DISPLAY || frame.layer_count != 2) {
    return FALSE;
  }
  if (!aa_video_texture_ensure_converter(self)) {
    g_dmabuf_supported.store(false);
    return FALSE;
  }

  GlStateGuard guard;

  // The output texture is bound on a unit of its own, and before the planes rather than
  // after. Every glBindTexture lands on whatever unit is active, so resizing the output
  // while unit 1 was current used to replace the chroma plane with the previous frame:
  // the picture decoded and displayed, in entirely the wrong colours.
  glActiveTexture(GL_TEXTURE2);
  aa_video_texture_resize_output(self, frame.width, frame.height, nullptr);

  glActiveTexture(GL_TEXTURE0);
  const EGLImageKHR luma =
      aa_video_texture_import_layer(display, frame.layers[0], self->plane_textures[0]);
  glActiveTexture(GL_TEXTURE1);
  const EGLImageKHR chroma =
      aa_video_texture_import_layer(display, frame.layers[1], self->plane_textures[1]);
  if (luma == EGL_NO_IMAGE_KHR || chroma == EGL_NO_IMAGE_KHR) {
    if (luma != EGL_NO_IMAGE_KHR) {
      eglDestroyImageKHR(display, luma);
    }
    if (chroma != EGL_NO_IMAGE_KHR) {
      eglDestroyImageKHR(display, chroma);
    }
    // One failure is enough to know this driver will not do it. Say so, so the decoder
    // reopens on the software backend instead of producing frames nothing can show.
    g_dmabuf_supported.store(false);
    g_warning("android_auto: EGL would not import the decoder's dmabuf, falling back to "
              "software decode.");
    return FALSE;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->name,
                         0);
  gboolean drawn = FALSE;
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
    glViewport(0, 0, frame.width, frame.height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);

    glBindVertexArray(self->vertex_array);
    glUseProgram(self->program);
    glUniform1i(self->uniform_luma, 0);
    glUniform1i(self->uniform_chroma, 1);

    const bool bt709 = frame.color_space == aa::ColorSpace::kBt709;
    const float* matrix = frame.full_range ? (bt709 ? kBt709Full : kBt601Full)
                                           : (bt709 ? kBt709Limited : kBt601Limited);
    glUniformMatrix3fv(self->uniform_matrix, 1, GL_FALSE, matrix);
    const float luma_offset = frame.full_range ? 0.0f : 16.0f / 255.0f;
    glUniform3f(self->uniform_offset, luma_offset, 128.0f / 255.0f, 128.0f / 255.0f);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    drawn = TRUE;
  } else {
    g_warning("android_auto: the video conversion framebuffer is not complete.");
  }

  // Detach before the output texture is handed to Flutter, so nothing is both a render
  // target and a sampled texture at the same time.
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  // The draw is issued, so the images have been consumed as far as the API is
  // concerned. The dmabuf behind them stays alive regardless: the FrameRing holds the
  // decoder's frame until the slot is written again, two frames from now.
  glFlush();
  eglDestroyImageKHR(display, chroma);
  eglDestroyImageKHR(display, luma);
  return drawn;
}

static gboolean aa_video_texture_populate(FlTextureGL* texture,
                                          uint32_t* target,
                                          uint32_t* name,
                                          uint32_t* width,
                                          uint32_t* height,
                                          GError** error) {
  AaVideoTexture* self = AA_VIDEO_TEXTURE(texture);
  aa_video_texture_probe(self);

  aa::Frame frame;
  if (self->ring != nullptr && self->ring->AcquireRead(&frame)) {
    if (frame.kind == aa::FrameKind::kDmabuf) {
      aa_video_texture_draw_dmabuf(self, frame);
    } else if (frame.kind == aa::FrameKind::kCpuRgba && frame.pixels != nullptr) {
      // Flutter only accepts GL_RGBA8, so producers on this path hand over RGBA and
      // there is nothing to convert.
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      aa_video_texture_resize_output(self, frame.width, frame.height, frame.pixels);
    }
    self->ring->ReleaseRead();
  }

  if (self->name == 0 || self->allocated_width == 0 || self->allocated_height == 0) {
    // Flutter asked to draw before anything was produced. Hand back a single
    // transparent pixel rather than an unallocated texture name, which some drivers
    // sample as garbage.
    const uint8_t transparent[4] = {0, 0, 0, 0};
    aa_video_texture_resize_output(self, 1, 1, transparent);
  } else {
    glBindTexture(GL_TEXTURE_2D, self->name);
  }

  *target = GL_TEXTURE_2D;
  *name = self->name;
  *width = static_cast<uint32_t>(self->allocated_width);
  *height = static_cast<uint32_t>(self->allocated_height);
  return TRUE;
}

static void aa_video_texture_dispose(GObject* object) {
  AaVideoTexture* self = AA_VIDEO_TEXTURE(object);
  // The GL context is not current here, so none of these can be deleted. Flutter tears
  // its context down on shutdown anyway, which reclaims the lot.
  self->name = 0;
  self->program = 0;
  self->framebuffer = 0;
  self->vertex_array = 0;
  self->plane_textures[0] = 0;
  self->plane_textures[1] = 0;
  self->converter_ready = FALSE;
  self->ring = nullptr;
  G_OBJECT_CLASS(aa_video_texture_parent_class)->dispose(object);
}

static void aa_video_texture_class_init(AaVideoTextureClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = aa_video_texture_populate;
  G_OBJECT_CLASS(klass)->dispose = aa_video_texture_dispose;
}

static void aa_video_texture_init(AaVideoTexture* self) {
  self->ring = nullptr;
  self->name = 0;
  self->allocated_width = 0;
  self->allocated_height = 0;
  self->converter_ready = FALSE;
  self->converter_failed = FALSE;
  self->program = 0;
  self->framebuffer = 0;
  self->vertex_array = 0;
  self->plane_textures[0] = 0;
  self->plane_textures[1] = 0;
  self->uniform_luma = -1;
  self->uniform_chroma = -1;
  self->uniform_matrix = -1;
  self->uniform_offset = -1;
}

static AaVideoTexture* aa_video_texture_new(aa::FrameRing* ring) {
  AaVideoTexture* self =
      AA_VIDEO_TEXTURE(g_object_new(aa_video_texture_get_type(), nullptr));
  self->ring = ring;
  return self;
}

namespace aa {

GlAdapter::GlAdapter(FrameRing* ring) : ring_(ring) {}

GlAdapter::~GlAdapter() { Shutdown(); }

bool GlAdapter::DmabufSupported() { return g_dmabuf_supported.load(); }

bool GlAdapter::Register() {
  if (texture_ != nullptr) {
    return true;
  }
  FlTextureRegistrar* registrar = GetTextureRegistrar();
  if (registrar == nullptr) {
    // The GTK plugin entry point has not run. That would mean the plugin was loaded
    // without Flutter's plugin registration, which should be impossible.
    return false;
  }

  AaVideoTexture* texture = aa_video_texture_new(ring_);
  if (!fl_texture_registrar_register_texture(registrar, FL_TEXTURE(texture))) {
    g_object_unref(texture);
    return false;
  }
  texture_ = texture;
  texture_id_ = fl_texture_get_id(FL_TEXTURE(texture));
  // Ask for a draw straight away. Nothing has been produced yet, so this paints one
  // transparent pixel, but it is what gets the raster thread to run the capability
  // probe before the decoder has to choose a backend.
  NotifyFrameAvailable();
  return true;
}

bool GlAdapter::NotifyFrameAvailable() {
  if (texture_ == nullptr) {
    return false;
  }
  return fl_texture_registrar_mark_texture_frame_available(
             GetTextureRegistrar(), FL_TEXTURE(static_cast<AaVideoTexture*>(texture_))) ==
         TRUE;
}

int64_t GlAdapter::texture_id() const { return texture_id_; }

void GlAdapter::Shutdown() {
  if (texture_ == nullptr) {
    return;
  }
  AaVideoTexture* texture = static_cast<AaVideoTexture*>(texture_);
  FlTextureRegistrar* registrar = GetTextureRegistrar();
  if (registrar != nullptr) {
    fl_texture_registrar_unregister_texture(registrar, FL_TEXTURE(texture));
  }
  g_object_unref(texture);
  texture_ = nullptr;
  texture_id_ = -1;
}

}  // namespace aa
