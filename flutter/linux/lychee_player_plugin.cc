#include "include/lychee_player/lychee_player_plugin.h"

#include <gtk/gtk.h>
#include <lychee/lychee.h>

#include <cstring>

// Bridges one lychee_player to a Flutter pixel buffer texture. The view's
// frame clock ticks once per display refresh on the GTK main thread; each
// tick asks the player whether the frame for that refresh is new, and if so
// tells Flutter, which then pulls it through copy_pixels on its raster
// thread. The mutex keeps the two threads and disposal apart.

G_DECLARE_FINAL_TYPE(LycheeTexture,
                     lychee_texture,
                     LYCHEE,
                     TEXTURE,
                     FlPixelBufferTexture)

struct _LycheeTexture {
  FlPixelBufferTexture parent_instance;

  GMutex mutex;
  lychee_player* player;      // Guarded by mutex.
  int64_t next_display_time;  // Guarded by mutex.

  // Raster thread only (and finalize). The frame whose pixels were last
  // handed to Flutter: they must stay valid until the next copy_pixels.
  lychee_frame* frame;
  // Tightly packed copy, for frames whose rows are padded.
  uint8_t* packed;
  size_t packed_size;

  // Main thread only.
  FlTextureRegistrar* registrar;
  GtkWidget* view;  // Weak.
  guint tick_id;
};

G_DEFINE_TYPE(LycheeTexture, lychee_texture, fl_pixel_buffer_texture_get_type())

static gboolean lychee_texture_copy_pixels(FlPixelBufferTexture* texture,
                                           const uint8_t** buffer,
                                           uint32_t* width,
                                           uint32_t* height,
                                           GError** error) {
  LycheeTexture* self = LYCHEE_TEXTURE(texture);
  g_mutex_lock(&self->mutex);
  if (self->player) {
    int64_t display_time = self->next_display_time;
    const int64_t now = lychee_now_us();
    if (display_time < now) {
      display_time = now;
    }
    lychee_frame* frame = lychee_player_acquire_frame(self->player, display_time);
    if (frame) {
      if (self->frame) {
        lychee_frame_release(self->frame);
      }
      self->frame = frame;
    }
  }
  g_mutex_unlock(&self->mutex);

  if (!self->frame) {
    // Flutter paints the texture once before the first frame is marked
    // available; show nothing rather than log an error.
    static const uint8_t kTransparent[4] = {0, 0, 0, 0};
    *buffer = kTransparent;
    *width = 1;
    *height = 1;
    return TRUE;
  }
  int32_t stride = 0;
  const uint8_t* data = lychee_frame_get_data(self->frame, &stride);
  const int32_t frame_width = lychee_frame_get_width(self->frame);
  const int32_t frame_height = lychee_frame_get_height(self->frame);
  if (!data || frame_width <= 0 || frame_height <= 0) {
    g_set_error_literal(error, g_quark_from_static_string("lychee_player"), 0,
                        "frame has no CPU pixels");
    return FALSE;
  }
  const size_t row = static_cast<size_t>(frame_width) * 4;
  if (static_cast<size_t>(stride) == row) {
    *buffer = data;
  } else {
    // glTexImage2D takes packed rows.
    const size_t size = row * frame_height;
    if (self->packed_size != size) {
      g_free(self->packed);
      self->packed = static_cast<uint8_t*>(g_malloc(size));
      self->packed_size = size;
    }
    for (int32_t y = 0; y < frame_height; ++y) {
      memcpy(self->packed + row * y, data + static_cast<size_t>(stride) * y,
             row);
    }
    *buffer = self->packed;
  }
  *width = static_cast<uint32_t>(frame_width);
  *height = static_cast<uint32_t>(frame_height);
  return TRUE;
}

static void lychee_texture_finalize(GObject* object) {
  LycheeTexture* self = LYCHEE_TEXTURE(object);
  if (self->frame) {
    lychee_frame_release(self->frame);
  }
  g_free(self->packed);
  g_mutex_clear(&self->mutex);
  G_OBJECT_CLASS(lychee_texture_parent_class)->finalize(object);
}

static void lychee_texture_class_init(LycheeTextureClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = lychee_texture_finalize;
  FL_PIXEL_BUFFER_TEXTURE_CLASS(klass)->copy_pixels =
      lychee_texture_copy_pixels;
}

static void lychee_texture_init(LycheeTexture* self) {
  g_mutex_init(&self->mutex);
}

// Main thread, once per frame of the view's frame clock.
static gboolean lychee_texture_on_tick(GtkWidget*,
                                       GdkFrameClock* clock,
                                       gpointer user_data) {
  LycheeTexture* self = LYCHEE_TEXTURE(user_data);
  // Frame clock times are g_get_monotonic_time(): CLOCK_MONOTONIC in
  // microseconds, the lychee_now_us() timebase. The predicted presentation
  // time is when what is drawn now reaches the screen.
  const gint64 frame_time = gdk_frame_clock_get_frame_time(clock);
  gint64 interval = 0;
  gint64 presentation = 0;
  gdk_frame_clock_get_refresh_info(clock, frame_time, &interval,
                                   &presentation);
  const int64_t display_time =
      presentation > 0 ? presentation : frame_time + interval;

  g_mutex_lock(&self->mutex);
  self->next_display_time = display_time;
  // Called every refresh, drawn or not: the player estimates the display's
  // refresh interval from these calls.
  const bool has_new_frame =
      self->player &&
      lychee_player_has_new_frame(self->player, display_time) != 0;
  g_mutex_unlock(&self->mutex);
  if (has_new_frame) {
    fl_texture_registrar_mark_texture_frame_available(self->registrar,
                                                      FL_TEXTURE(self));
  }
  return G_SOURCE_CONTINUE;
}

static LycheeTexture* lychee_texture_new(lychee_player* player,
                                         FlTextureRegistrar* registrar,
                                         GtkWidget* view) {
  LycheeTexture* self =
      LYCHEE_TEXTURE(g_object_new(lychee_texture_get_type(), nullptr));
  self->player = player;
  self->registrar = registrar;
  if (view) {
    self->view = view;
    g_object_add_weak_pointer(G_OBJECT(view),
                              reinterpret_cast<gpointer*>(&self->view));
    self->tick_id =
        gtk_widget_add_tick_callback(view, lychee_texture_on_tick, self,
                                     nullptr);
  } else {
    g_warning("lychee_player: no Flutter view, video will not update");
  }
  return self;
}

// Main thread. After this returns the player is never touched again and may
// be destroyed.
static void lychee_texture_dispose_player(LycheeTexture* self) {
  if (self->view) {
    // Tick callbacks run on this thread, so none is running now.
    gtk_widget_remove_tick_callback(self->view, self->tick_id);
    g_object_remove_weak_pointer(G_OBJECT(self->view),
                                 reinterpret_cast<gpointer*>(&self->view));
    self->view = nullptr;
  }
  g_mutex_lock(&self->mutex);
  self->player = nullptr;
  g_mutex_unlock(&self->mutex);
}

G_DECLARE_FINAL_TYPE(LycheePlayerPlugin,
                     lychee_player_plugin,
                     LYCHEE,
                     PLAYER_PLUGIN,
                     GObject)

struct _LycheePlayerPlugin {
  GObject parent_instance;
  // Weak: the registrar owns the messenger, which owns this plugin through
  // the channel handler; a strong reference would be a cycle.
  GWeakRef view;
  FlTextureRegistrar* registrar;
  // Texture id -> LycheeTexture (owned).
  GHashTable* textures;
};

G_DEFINE_TYPE(LycheePlayerPlugin, lychee_player_plugin, G_TYPE_OBJECT)

static gboolean release_texture(gpointer texture) {
  g_object_unref(texture);
  return G_SOURCE_REMOVE;
}

static FlMethodResponse* create_texture(LycheePlayerPlugin* self,
                                        FlValue* args) {
  FlValue* player_value =
      args && fl_value_get_type(args) == FL_VALUE_TYPE_MAP
          ? fl_value_lookup_string(args, "player")
          : nullptr;
  if (!player_value || fl_value_get_type(player_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("bad_args", "player missing", nullptr));
  }
  auto* player =
      reinterpret_cast<lychee_player*>(fl_value_get_int(player_value));
  g_autoptr(GObject) view = static_cast<GObject*>(g_weak_ref_get(&self->view));
  LycheeTexture* texture = lychee_texture_new(
      player, self->registrar, view ? GTK_WIDGET(view) : nullptr);
  if (!fl_texture_registrar_register_texture(self->registrar,
                                             FL_TEXTURE(texture))) {
    lychee_texture_dispose_player(texture);
    g_object_unref(texture);
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "texture", "cannot register texture", nullptr));
  }
  const int64_t id = fl_texture_get_id(FL_TEXTURE(texture));
  g_hash_table_insert(self->textures, GINT_TO_POINTER(id), texture);
  g_autoptr(FlValue) result = fl_value_new_int(id);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse* dispose_texture(LycheePlayerPlugin* self,
                                         FlValue* args) {
  FlValue* id_value = args && fl_value_get_type(args) == FL_VALUE_TYPE_MAP
                          ? fl_value_lookup_string(args, "textureId")
                          : nullptr;
  if (!id_value || fl_value_get_type(id_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("bad_args", "textureId missing", nullptr));
  }
  const int64_t id = fl_value_get_int(id_value);
  gpointer texture = nullptr;
  if (g_hash_table_steal_extended(self->textures, GINT_TO_POINTER(id), nullptr,
                                  &texture)) {
    lychee_texture_dispose_player(LYCHEE_TEXTURE(texture));
    fl_texture_registrar_unregister_texture(self->registrar,
                                            FL_TEXTURE(texture));
    // The raster thread looks textures up without a reference and may still
    // be inside copy_pixels; keep ours a while so it does not see the
    // texture freed under it.
    g_timeout_add_seconds(1, release_texture, texture);
  }
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static void method_call_cb(FlMethodChannel*,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  LycheePlayerPlugin* self = LYCHEE_PLAYER_PLUGIN(user_data);
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);
  g_autoptr(FlMethodResponse) response = nullptr;
  if (strcmp(method, "createTexture") == 0) {
    response = create_texture(self, args);
  } else if (strcmp(method, "disposeTexture") == 0) {
    response = dispose_texture(self, args);
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }
  fl_method_call_respond(method_call, response, nullptr);
}

static void dispose_texture_entry(gpointer texture) {
  lychee_texture_dispose_player(LYCHEE_TEXTURE(texture));
  g_object_unref(texture);
}

static void lychee_player_plugin_dispose(GObject* object) {
  LycheePlayerPlugin* self = LYCHEE_PLAYER_PLUGIN(object);
  g_clear_pointer(&self->textures, g_hash_table_unref);
  g_clear_object(&self->registrar);
  g_weak_ref_clear(&self->view);
  G_OBJECT_CLASS(lychee_player_plugin_parent_class)->dispose(object);
}

static void lychee_player_plugin_class_init(LycheePlayerPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = lychee_player_plugin_dispose;
}

static void lychee_player_plugin_init(LycheePlayerPlugin* self) {
  g_weak_ref_init(&self->view, nullptr);
  self->textures = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         nullptr, dispose_texture_entry);
}

void lychee_player_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  LycheePlayerPlugin* plugin = LYCHEE_PLAYER_PLUGIN(
      g_object_new(lychee_player_plugin_get_type(), nullptr));
  // The runner creates the view before registering plugins; NULL when
  // headless.
  g_weak_ref_set(&plugin->view, fl_plugin_registrar_get_view(registrar));
  plugin->registrar = FL_TEXTURE_REGISTRAR(
      g_object_ref(fl_plugin_registrar_get_texture_registrar(registrar)));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "lychee_player", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);
  g_object_unref(plugin);
}
