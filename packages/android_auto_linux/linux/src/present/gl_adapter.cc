#include "gl_adapter.h"

#include <epoxy/gl.h>
#include <flutter_linux/flutter_linux.h>

#include "../frame_ring.h"
#include "texture_registry.h"

// An FlTextureGL that uploads whatever the FrameRing currently holds.
//
// Flutter calls populate() on its raster thread with its own GL context already
// current, which is the one place in this project where GL calls are legal. Nothing
// here allocates or takes a lock across a GL call.
struct _AaVideoTexture {
  FlTextureGL parent_instance;
  aa::FrameRing* ring;
  GLuint name;
  // Dimensions the GL texture was last allocated at. A change means glTexImage2D
  // instead of the cheaper glTexSubImage2D.
  int32_t allocated_width;
  int32_t allocated_height;
};

G_DECLARE_FINAL_TYPE(AaVideoTexture, aa_video_texture, AA, VIDEO_TEXTURE, FlTextureGL)
G_DEFINE_TYPE(AaVideoTexture, aa_video_texture, fl_texture_gl_get_type())

static gboolean aa_video_texture_populate(FlTextureGL* texture,
                                          uint32_t* target,
                                          uint32_t* name,
                                          uint32_t* width,
                                          uint32_t* height,
                                          GError** error) {
  AaVideoTexture* self = AA_VIDEO_TEXTURE(texture);

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

  aa::Frame frame;
  if (self->ring != nullptr && self->ring->AcquireRead(&frame)) {
    // Flutter only accepts GL_RGBA8, so producers hand over RGBA and there is no
    // conversion to do here. M4 adds the dmabuf branch alongside this one.
    if (frame.kind == aa::FrameKind::kCpuRgba && frame.pixels != nullptr) {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      if (frame.width != self->allocated_width ||
          frame.height != self->allocated_height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, frame.pixels);
        self->allocated_width = frame.width;
        self->allocated_height = frame.height;
      } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_RGBA,
                        GL_UNSIGNED_BYTE, frame.pixels);
      }
    }
    self->ring->ReleaseRead();
  }

  if (self->allocated_width == 0 || self->allocated_height == 0) {
    // Flutter asked to draw before anything was produced. Hand back a single
    // transparent pixel rather than an unallocated texture name, which some drivers
    // sample as garbage.
    const uint8_t transparent[4] = {0, 0, 0, 0};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 transparent);
    self->allocated_width = 1;
    self->allocated_height = 1;
  }

  *target = GL_TEXTURE_2D;
  *name = self->name;
  *width = static_cast<uint32_t>(self->allocated_width);
  *height = static_cast<uint32_t>(self->allocated_height);
  return TRUE;
}

static void aa_video_texture_dispose(GObject* object) {
  AaVideoTexture* self = AA_VIDEO_TEXTURE(object);
  // The GL context is not current here, so the texture name cannot be deleted. Flutter
  // tears its context down on shutdown anyway, which reclaims it.
  self->name = 0;
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
