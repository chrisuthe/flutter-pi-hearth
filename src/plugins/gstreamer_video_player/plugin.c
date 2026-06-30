#define _GNU_SOURCE

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>

#include <gst/gst.h>
#include <gst/video/navigation.h>
#include <gst/video/video-info.h>

#include "flutter-pi.h"
#include "notifier_listener.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "plugins/gstreamer_video_player.h"
#include "texture_registry.h"
#include "util/collection.h"
#include "util/list.h"
#include "util/logging.h"

enum data_source_type { kDataSourceTypeAsset, kDataSourceTypeNetwork, kDataSourceTypeFile, kDataSourceTypeContentUri };

struct gstplayer_meta {
    struct list_head entry;

    struct gstplayer *player;

    char *event_channel_name;

    // We have a listener to the video player event channel.
    bool has_listener;

    /*
    sd_event_source *probe_video_info_source;
    bool has_video_info;
    bool is_stream;
    int64_t duration_ms;
    int32_t width, height;
    */

    atomic_bool is_buffering;

    struct listener *video_info_listener;
    struct listener *buffering_state_listener;
    struct listener *error_listener;
};

/// Global event channel name used to broadcast pipeline errors (e.g. wpesrc
/// failures, WebProcess crashes) to all Dart-side subscribers. Events sent on
/// this channel are maps of `{textureId: int, message: string}`. Used by the
/// Hearth webview integration; safe to ignore for normal video playback.
#define ERROR_EVENT_CHANNEL "flutterpi_gstreamer_video_player/error"

static struct plugin {
    pthread_mutex_t lock;

    struct flutterpi *flutterpi;
    bool initialized;
    struct list_head players;

    /// True iff the Dart side currently has a subscription on
    /// @ref ERROR_EVENT_CHANNEL. Used to skip the platch_send call when
    /// nothing is listening.
    bool has_error_listener;
} plugin;

DEFINE_LOCK_OPS(plugin, lock);

/// Add a player instance to the player collection.
static void add_player(struct gstplayer_meta *meta) {
    plugin_lock(&plugin);

    list_add(&meta->entry, &plugin.players);

    plugin_unlock(&plugin);
}

/// Get a player instance by its id.
static struct gstplayer *get_player_by_texture_id(int64_t texture_id) {
    plugin_lock(&plugin);

    list_for_each_entry(struct gstplayer_meta, meta, &plugin.players, entry) {
        if (gstplayer_get_texture_id(meta->player) == texture_id) {
            plugin_unlock(&plugin);
            return meta->player;
        }
    }

    plugin_unlock(&plugin);
    return NULL;
}

/// Get a player instance by its event channel name.
static struct gstplayer *get_player_by_evch(const char *const event_channel_name) {
    plugin_lock(&plugin);

    list_for_each_entry(struct gstplayer_meta, meta, &plugin.players, entry) {
        if (streq(meta->event_channel_name, event_channel_name)) {
            plugin_unlock(&plugin);
            return meta->player;
        }
    }

    plugin_unlock(&plugin);
    return NULL;
}

/**
 * @brief Remove a player instance from the player list.
 * 
 * Assumes the plugin struct is not locked.
 * 
 */
static void remove_player(struct gstplayer_meta *meta) {
    plugin_lock(&plugin);

    list_del(&meta->entry);

    plugin_unlock(&plugin);
}

/**
 * @brief Remove a player instance from the player list.
 * 
 * Assumes the plugin struct is locked.
 * 
 */
static void remove_player_locked(struct gstplayer_meta *meta) {
    ASSERT_MUTEX_LOCKED(plugin.lock);
    list_del(&meta->entry);
}

static struct gstplayer_meta *get_meta(struct gstplayer *player) {
    return (struct gstplayer_meta *) gstplayer_get_userdata_locked(player);
}

/// Get the player id from the given arg, which is a kStdMap.
/// (*texture_id_out = arg['playerId'])
/// If an error ocurrs, this will respond with an illegal argument error to the given responsehandle.
static int
get_texture_id_from_map_arg(struct std_value *arg, int64_t *texture_id_out, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct std_value *id;
    int ok;

    if (!STDVALUE_IS_MAP(*arg)) {
        ok = platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg` to be a Map, but was: ", arg);
        if (ok != 0)
            return ok;

        return EINVAL;
    }

    id = stdmap_get_str(arg, "textureId");
    if (id == NULL || !STDVALUE_IS_INT(*id)) {
        ok = platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg['textureId']` to be an integer, but was: ", id);
        if (ok != 0)
            return ok;

        return EINVAL;
    }

    *texture_id_out = STDVALUE_AS_INT(*id);

    return 0;
}

/// Get the player associated with the id in the given arg, which is a kStdMap.
/// (*player_out = get_player_by_texture_id(get_texture_id_from_map_arg(arg)))
/// If an error ocurrs, this will respond with an illegal argument error to the given responsehandle.
static int
get_player_from_map_arg(struct std_value *arg, struct gstplayer **player_out, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    int64_t texture_id;
    int ok;

    texture_id = 0;
    ok = get_texture_id_from_map_arg(arg, &texture_id, responsehandle);
    if (ok != 0) {
        return ok;
    }

    player = get_player_by_texture_id(texture_id);
    if (player == NULL) {
        plugin_lock(&plugin);

        int n_texture_ids = list_length(&plugin.players);
        int64_t *texture_ids = alloca(sizeof(int64_t) * n_texture_ids);
        int64_t *texture_ids_cursor = texture_ids;

        list_for_each_entry(struct gstplayer_meta, meta, &plugin.players, entry) {
            *texture_ids_cursor++ = gstplayer_get_texture_id(meta->player);
        }

        plugin_unlock(&plugin);

        ok = platch_respond_illegal_arg_ext_pigeon(
            responsehandle,
            "Expected `arg['textureId']` to be a valid texture id.",
            &STDMAP2(
                STDSTRING("textureId"),
                STDINT64(texture_id),
                STDSTRING("registeredTextureIds"),
                ((struct std_value){ .type = kStdInt64Array, .size = n_texture_ids, .int64array = texture_ids })
            )
        );
        if (ok != 0)
            return ok;

        return EINVAL;
    }

    *player_out = player;

    return 0;
}

static int ensure_initialized() {
    GError *gst_error;
    gboolean success;

    if (plugin.initialized) {
        return 0;
    }

    success = gst_init_check(NULL, NULL, &gst_error);
    if (!success) {
        LOG_ERROR("Could not initialize gstreamer: %s\n", gst_error->message);
        return gst_error->code;
    }

    plugin.initialized = true;
    return 0;
}

static int respond_init_failed(FlutterPlatformMessageResponseHandle *handle) {
    return platch_respond_error_pigeon(
        handle,
        "couldnotinit",
        "gstreamer video player plugin failed to initialize gstreamer. See flutter-pi log for details.",
        NULL
    );
}

static int respond_init_failed_v2(FlutterPlatformMessageResponseHandle *handle) {
    return platch_respond_error_std(
        handle,
        "couldnotinit",
        "gstreamer video player plugin failed to initialize gstreamer. See flutter-pi log for details.",
        NULL
    );
}

static int send_initialized_event(struct gstplayer_meta *meta, bool is_stream, int width, int height, int64_t duration_ms) {
    return platch_send_success_event_std(
        meta->event_channel_name,
        &STDMAP4(
            STDSTRING("event"),
            STDSTRING("initialized"),
            STDSTRING("duration"),
            STDINT64(is_stream ? INT64_MAX : duration_ms),
            STDSTRING("width"),
            STDINT32(width),
            STDSTRING("height"),
            STDINT32(height)
        )
    );
}

UNUSED static int send_completed_event(struct gstplayer_meta *meta) {
    return platch_send_success_event_std(meta->event_channel_name, &STDMAP1(STDSTRING("event"), STDSTRING("completed")));
}

static int send_buffering_update(struct gstplayer_meta *meta, int n_ranges, const struct buffering_range *ranges) {
    struct std_value values;

    values.type = kStdList;
    values.size = n_ranges;
    values.list = alloca(sizeof(struct std_value) * n_ranges);

    for (size_t i = 0; i < n_ranges; i++) {
        values.list[i].type = kStdList;
        values.list[i].size = 2;
        values.list[i].list = alloca(sizeof(struct std_value) * 2);

        values.list[i].list[0] = STDINT32(ranges[i].start_ms);
        values.list[i].list[1] = STDINT32(ranges[i].stop_ms);
    }

    return platch_send_success_event_std(
        meta->event_channel_name,
        &STDMAP2(STDSTRING("event"), STDSTRING("bufferingUpdate"), STDSTRING("values"), values)
    );
}

static int send_buffering_start(struct gstplayer_meta *meta) {
    return platch_send_success_event_std(meta->event_channel_name, &STDMAP1(STDSTRING("event"), STDSTRING("bufferingStart")));
}

static int send_buffering_end(struct gstplayer_meta *meta) {
    return platch_send_success_event_std(meta->event_channel_name, &STDMAP1(STDSTRING("event"), STDSTRING("bufferingEnd")));
}

static enum listener_return on_video_info_notify(void *arg, void *userdata) {
    struct gstplayer_meta *meta;
    struct video_info *info;

    ASSERT_NOT_NULL(userdata);
    meta = userdata;
    info = arg;

    // When the video info is not known yet, we still get informed about it.
    // In that case arg == NULL.
    if (arg == NULL) {
        return kNoAction;
    }

    LOG_DEBUG(
        "Got video info: stream? %s, w x h: % 4d x % 4d, duration: %" GST_TIME_FORMAT "\n",
        !info->can_seek ? "yes" : "no",
        info->width,
        info->height,
        GST_TIME_ARGS(info->duration_ms * GST_MSECOND)
    );

    /// on_video_info_notify is called on an internal thread,
    /// but send_initialized_event is (should be) mt-safe
    send_initialized_event(meta, !info->can_seek, info->width, info->height, info->duration_ms);
    
    /// FIXME: Threading
    /// Set this to NULL here so we don't unlisten to it twice.
    meta->video_info_listener = NULL;

    /// TODO: We should only send the initialized event once,
    /// but maybe it's also okay if we send it multiple times?
    return kUnlisten;
}

static enum listener_return on_buffering_state_notify(void *arg, void *userdata) {
    struct buffering_state *state;
    struct gstplayer_meta *meta;
    bool new_is_buffering;

    ASSERT_NOT_NULL(userdata);
    meta = userdata;
    state = arg;

    if (arg == NULL) {
        return kNoAction;
    }

    new_is_buffering = state->percent != 100;

    if (meta->is_buffering && !new_is_buffering) {
        send_buffering_end(meta);
        meta->is_buffering = false;
    } else if (!meta->is_buffering && new_is_buffering) {
        send_buffering_start(meta);
        meta->is_buffering = true;
    }

    send_buffering_update(meta, state->n_ranges, state->ranges);
    return kNoAction;
}

/// Listener callback for gstplayer error notifications. Forwards the error
/// message to the Dart side via the global error event channel, tagged with
/// the texture id so multi-player apps can route the event.
///
/// Called on the gstreamer bus-watch thread; @ref platch_send_success_event_std
/// is expected to be MT-safe (matches the existing pattern used by
/// @ref send_initialized_event from the same thread).
static enum listener_return on_error_notify(void *arg, void *userdata) {
    struct gstplayer_meta *meta;
    const char *message;

    ASSERT_NOT_NULL(userdata);
    meta = userdata;

    // Value-notifier replay may pass NULL on initial subscribe (before any
    // error has actually happened). Ignore those.
    if (arg == NULL) {
        return kNoAction;
    }
    message = arg;

    if (plugin.has_error_listener) {
        platch_send_success_event_std(
            ERROR_EVENT_CHANNEL,
            &STDMAP2(
                STDSTRING("textureId"),
                STDINT64(gstplayer_get_texture_id(meta->player)),
                STDSTRING("message"),
                STDSTRING((char *) message)
            )
        );
    }

    return kNoAction;
}

/// Receiver for the global error event channel. Handles the standard Flutter
/// EventChannel `listen` / `cancel` protocol so the Dart side can subscribe
/// to pipeline errors.
static int on_receive_error_evch(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    const char *method;

    (void) channel;

    method = object->method;

    if (streq("listen", method)) {
        plugin.has_error_listener = true;
        return platch_respond_success_std(responsehandle, NULL);
    } else if (streq("cancel", method)) {
        plugin.has_error_listener = false;
        return platch_respond_success_std(responsehandle, NULL);
    } else {
        return platch_respond_not_implemented(responsehandle);
    }
}

/*******************************************************
 * CHANNEL HANDLERS                                    *
 * handle method calls on the method and event channel *
 *******************************************************/
static int on_receive_evch(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer_meta *meta;
    struct gstplayer *player;
    const char *method;

    method = object->method;

    player = get_player_by_evch(channel);
    if (player == NULL) {
        return platch_respond_not_implemented(responsehandle);
    }

    meta = gstplayer_get_userdata_locked(player);

    if (streq("listen", method)) {
        platch_respond_success_std(responsehandle, NULL);
        meta->has_listener = true;

        meta->video_info_listener = notifier_listen(gstplayer_get_video_info_notifier(player), on_video_info_notify, NULL, meta);
        // We don't care if it's NULL, it could also be on_video_info_notify was called synchronously. (And returned kUnlisten)

        meta->buffering_state_listener =
            notifier_listen(gstplayer_get_buffering_state_notifier(player), on_buffering_state_notify, NULL, meta);
        if (meta->buffering_state_listener == NULL) {
            LOG_ERROR("Couldn't listen for buffering events in gstplayer.\n");
        }
    } else if (streq("cancel", method)) {
        platch_respond_success_std(responsehandle, NULL);
        meta->has_listener = false;

        if (meta->video_info_listener != NULL) {
            notifier_unlisten(gstplayer_get_video_info_notifier(player), meta->video_info_listener);
            meta->video_info_listener = NULL;
        }
        if (meta->buffering_state_listener != NULL) {
            notifier_unlisten(gstplayer_get_buffering_state_notifier(player), meta->buffering_state_listener);
            meta->buffering_state_listener = NULL;
        }
    } else {
        return platch_respond_not_implemented(responsehandle);
    }

    return 0;
}

static int on_initialize(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    int ok;

    (void) channel;
    (void) object;

    ok = ensure_initialized();
    if (ok != 0) {
        return respond_init_failed_v2(responsehandle);
    }

    // what do we even do here?

    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int check_headers(const struct std_value *headers, FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct std_value *key, *value;

    if (headers == NULL || STDVALUE_IS_NULL(*headers)) {
        return 0;
    } else if (!STDVALUE_IS_MAP(*headers)) {
        platch_respond_illegal_arg_pigeon(responsehandle, "Expected `arg['httpHeaders']` to be a map of strings or null.");
        return EINVAL;
    }

    for (int i = 0; i < headers->size; i++) {
        key = headers->keys + i;
        value = headers->values + i;

        if (STDVALUE_IS_NULL(*key) || STDVALUE_IS_NULL(*value)) {
            // ignore this value
            continue;
        } else if (STDVALUE_IS_STRING(*key) && STDVALUE_IS_STRING(*value)) {
            // valid too
            continue;
        } else {
            platch_respond_illegal_arg_pigeon(responsehandle, "Expected `arg['httpHeaders']` to be a map of strings or null.");
            return EINVAL;
        }
    }

    return 0;
}

static int add_headers_to_player(const struct std_value *headers, struct gstplayer *player) {
    const struct std_value *key, *value;

    if (headers == NULL || STDVALUE_IS_NULL(*headers)) {
        return 0;
    } else if (!STDVALUE_IS_MAP(*headers)) {
        assert(false);
    }

    for (int i = 0; i < headers->size; i++) {
        key = headers->keys + i;
        value = headers->values + i;

        if (STDVALUE_IS_NULL(*key) || STDVALUE_IS_NULL(*value)) {
            // ignore this value
            continue;
        } else if (STDVALUE_IS_STRING(*key) && STDVALUE_IS_STRING(*value)) {
            gstplayer_put_http_header(player, STDVALUE_AS_STRING(*key), STDVALUE_AS_STRING(*value));
        } else {
            assert(false);
        }
    }

    return 0;
}

/// Allocates and initializes a gstplayer_meta struct, which we
/// use to store additional information in a gstplayer instance.
/// (The event channel name for that player)
static struct gstplayer_meta *create_meta(int64_t texture_id, struct gstplayer *player) {
    struct gstplayer_meta *meta;
    char *event_channel_name;
    int ok;

    meta = malloc(sizeof *meta);
    if (meta == NULL) {
        return NULL;
    }

    ok = asprintf(&event_channel_name, "flutter.io/videoPlayer/videoEvents%" PRId64, texture_id);
    if (ok < 0) {
        free(meta);
        return NULL;
    }

    meta->player = player;
    list_inithead(&meta->entry);
    meta->event_channel_name = event_channel_name;
    meta->has_listener = false;
    meta->is_buffering = false;
    meta->video_info_listener = NULL;
    meta->buffering_state_listener = NULL;
    meta->error_listener = NULL;
    return meta;
}

static void destroy_meta(struct gstplayer_meta *meta) {
    free(meta->event_channel_name);
    free(meta);
}

static void dispose_player(struct gstplayer *player, bool plugin_registry_locked, bool plugin_locked) {
    struct gstplayer_meta *meta;

    meta = get_meta(player);

    if (plugin_registry_locked) {
        plugin_registry_remove_receiver_locked(meta->event_channel_name);
    } else {
        plugin_registry_remove_receiver(meta->event_channel_name);
    }

    if (plugin_locked) {
        remove_player_locked(meta);
    } else {
        remove_player(meta);
    }

    if (meta->video_info_listener != NULL) {
        notifier_unlisten(gstplayer_get_video_info_notifier(player), meta->video_info_listener);
        meta->video_info_listener = NULL;
    }
    if (meta->buffering_state_listener != NULL) {
        notifier_unlisten(gstplayer_get_buffering_state_notifier(player), meta->buffering_state_listener);
        meta->buffering_state_listener = NULL;
    }
    if (meta->error_listener != NULL) {
        notifier_unlisten(gstplayer_get_error_notifier(player), meta->error_listener);
        meta->error_listener = NULL;
    }

    destroy_meta(meta);

    gstplayer_destroy(player);
}

/// Creates a new video player.
/// Should respond to the platform message when the player has established its viewport.
static int on_create(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer_meta *meta;
    struct gstplayer *player;
    struct std_value *arg, *temp;
    enum format_hint format_hint;
    char *asset, *uri, *package_name;
    int ok;

    (void) channel;

    arg = &(object->std_value);

    ok = ensure_initialized();
    if (ok != 0) {
        return respond_init_failed(responsehandle);
    }

    if (!STDVALUE_IS_MAP(*arg)) {
        return platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg` to be a Map, but was:", arg);
    }

    temp = stdmap_get_str(arg, "asset");
    if (temp == NULL || STDVALUE_IS_NULL(*temp)) {
        asset = NULL;
    } else if (STDVALUE_IS_STRING(*temp)) {
        asset = STDVALUE_AS_STRING(*temp);
    } else {
        return platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg['asset']` to be a String or null, but was:", temp);
    }

    temp = stdmap_get_str(arg, "uri");
    if (temp == NULL || STDVALUE_IS_NULL(*temp)) {
        uri = NULL;
    } else if (STDVALUE_IS_STRING(*temp)) {
        uri = STDVALUE_AS_STRING(*temp);
    } else {
        return platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg['uri']` to be a String or null, but was:", temp);
    }

    temp = stdmap_get_str(arg, "packageName");
    if (temp == NULL || STDVALUE_IS_NULL(*temp)) {
        package_name = NULL;
    } else if (STDVALUE_IS_STRING(*temp)) {
        package_name = STDVALUE_AS_STRING(*temp);
    } else {
        return platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg['packageName']` to be a String or null, but was:", temp);
    }

    temp = stdmap_get_str(arg, "formatHint");
    if (temp == NULL || STDVALUE_IS_NULL(*temp)) {
        format_hint = FORMAT_HINT_NONE;
    } else if (STDVALUE_IS_STRING(*temp)) {
        char *format_hint_str = STDVALUE_AS_STRING(*temp);

        if (streq("ss", format_hint_str)) {
            format_hint = FORMAT_HINT_SS;
        } else if (streq("hls", format_hint_str)) {
            format_hint = FORMAT_HINT_HLS;
        } else if (streq("dash", format_hint_str)) {
            format_hint = FORMAT_HINT_MPEG_DASH;
        } else if (streq("other", format_hint_str)) {
            format_hint = FORMAT_HINT_OTHER;
        } else {
            goto invalid_format_hint;
        }
    } else {
invalid_format_hint:

        return platch_respond_illegal_arg_ext_pigeon(
            responsehandle,
            "Expected `arg['formatHint']` to be one of 'ss', 'hls', 'dash', 'other' or null, but was:",
            temp
        );
    }

    temp = stdmap_get_str(arg, "httpHeaders");

    // check our headers are valid, so we don't create our player for nothing
    ok = check_headers(temp, responsehandle);
    if (ok != 0) {
        return 0;
    }

    // create our actual player (this doesn't initialize it)
    if (asset != NULL) {
        player = gstplayer_new_from_asset(flutterpi, asset, package_name, NULL);
    } else {
        player = gstplayer_new_from_network(flutterpi, uri, format_hint, NULL);
    }
    if (player == NULL) {
        LOG_ERROR("Couldn't create gstreamer video player.\n");
        ok = EIO;
        goto fail_respond_error;
    }

    // create a meta object so we can store the event channel name
    // of a player with it
    meta = create_meta(gstplayer_get_texture_id(player), player);
    if (meta == NULL) {
        ok = ENOMEM;
        goto fail_destroy_player;
    }

    gstplayer_set_userdata_locked(player, meta);

    // Add all our HTTP headers to gstplayer using gstplayer_put_http_header
    add_headers_to_player(temp, player);

    // add it to our player collection
    add_player(meta);

    // set a receiver on the videoEvents event channel
    ok = plugin_registry_set_receiver(meta->event_channel_name, kStandardMethodCall, on_receive_evch);
    if (ok != 0) {
        goto fail_remove_player;
    }

    // Subscribe to the player's error notifier. See the v2 create path for
    // rationale. Non-fatal if it fails.
    meta->error_listener = notifier_listen(gstplayer_get_error_notifier(player), on_error_notify, NULL, meta);
    if (meta->error_listener == NULL) {
        LOG_ERROR("Couldn't subscribe to gstplayer error notifier.\n");
    }

    // Finally, start initializing
    ok = gstplayer_initialize(player);
    if (ok != 0) {
        goto fail_remove_receiver;
    }

    return platch_respond_success_pigeon(responsehandle, &STDMAP1(STDSTRING("textureId"), STDINT64(gstplayer_get_texture_id(player))));

fail_remove_receiver:
    if (meta->error_listener != NULL) {
        notifier_unlisten(gstplayer_get_error_notifier(player), meta->error_listener);
        meta->error_listener = NULL;
    }
    plugin_registry_remove_receiver(meta->event_channel_name);

fail_remove_player:
    remove_player(meta);
    destroy_meta(meta);

fail_destroy_player:
    gstplayer_destroy(player);

fail_respond_error:
    return platch_respond_native_error_pigeon(responsehandle, ok);
}

static int on_dispose(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    struct std_value *arg;
    int ok;

    (void) channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return 0;
    }

    dispose_player(player, false, false);

    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_set_looping(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    struct std_value *arg, *temp;
    bool loop;
    int ok;

    (void) channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return ok;

    temp = stdmap_get_str(arg, "isLooping");
    if (temp && STDVALUE_IS_BOOL(*temp)) {
        loop = STDVALUE_AS_BOOL(*temp);
    } else {
        return platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg['isLooping']` to be a boolean, but was:", temp);
    }

    gstplayer_set_looping(player, loop);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_set_volume(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    struct std_value *arg, *temp;
    double volume;
    int ok;

    (void) channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return ok;

    temp = stdmap_get_str(arg, "volume");
    if (STDVALUE_IS_FLOAT(*temp)) {
        volume = STDVALUE_AS_FLOAT(*temp);
    } else {
        return platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg['volume']` to be a float/double, but was:", temp);
    }

    gstplayer_set_volume(player, volume);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_set_playback_speed(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    struct std_value *arg, *temp;
    double speed;
    int ok;

    (void) channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return ok;

    temp = stdmap_get_str(arg, "speed");
    if (STDVALUE_IS_FLOAT(*temp)) {
        speed = STDVALUE_AS_FLOAT(*temp);
    } else {
        return platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg['speed']` to be a float/double, but was:", temp);
    }

    gstplayer_set_playback_speed(player, speed);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_play(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    struct std_value *arg;
    int ok;

    (void) channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return 0;

    gstplayer_play(player);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_get_position(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    struct std_value *arg;
    int64_t position;
    int ok;

    (void) channel;
    (void) position;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return 0;

    position = gstplayer_get_position(player);

    if (position >= 0) {
        return platch_respond_success_pigeon(responsehandle, &STDMAP1(STDSTRING("position"), STDINT64(position)));
    } else {
        return platch_respond_error_pigeon(responsehandle, "native-error", "An unexpected gstreamer error ocurred.", NULL);
    }
}

static int on_seek_to(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    struct std_value *arg, *temp;
    int64_t position;
    int ok;

    (void) channel;

    arg = &(object->std_value);

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return 0;

    temp = stdmap_get_str(arg, "position");
    if (STDVALUE_IS_INT(*temp)) {
        position = STDVALUE_AS_INT(*temp);
    } else {
        return platch_respond_illegal_arg_pigeon(responsehandle, "Expected `arg['position']` to be an integer.");
    }

    gstplayer_seek_to(player, position, false);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_pause(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    struct std_value *arg;
    int ok;

    (void) channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return 0;

    gstplayer_pause(player);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_set_mix_with_others(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct std_value *arg;

    (void) channel;

    arg = &object->std_value;

    (void) arg;

    /// TODO: Should we do anything other here than just returning?
    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_step_forward(struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return 0;
    }

    ok = gstplayer_step_forward(player);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int on_step_backward(struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return 0;
    }

    ok = gstplayer_step_backward(player);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int on_fast_seek(struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    struct std_value *temp;
    int64_t position;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return 0;
    }

    temp = stdmap_get_str(arg, "position");
    if (STDVALUE_IS_INT(*temp)) {
        position = STDVALUE_AS_INT(*temp);
    } else {
        return platch_respond_illegal_arg_pigeon(responsehandle, "Expected `arg['position']` to be an integer.");
    }

    ok = gstplayer_seek_to(player, position, true);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int on_receive_method_channel(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    const char *method;

    (void) channel;

    method = object->method;

    if (streq("stepForward", method)) {
        return on_step_forward(&object->std_arg, responsehandle);
    } else if (streq("stepBackward", method)) {
        return on_step_backward(&object->std_arg, responsehandle);
    } else if (streq("fastSeek", method)) {
        return on_fast_seek(&object->std_arg, responsehandle);
    } else {
        return platch_respond_not_implemented(responsehandle);
    }
}

static struct gstplayer *
get_player_from_texture_id_with_custom_errmsg(int64_t texture_id, FlutterPlatformMessageResponseHandle *responsehandle, char *error_message) {
    struct gstplayer *player;

    player = get_player_by_texture_id(texture_id);
    if (player == NULL) {
        plugin_lock(&plugin);

        int n_texture_ids = list_length(&plugin.players);
        int64_t *texture_ids = alloca(sizeof(int64_t) * n_texture_ids);
        int64_t *texture_ids_cursor = texture_ids;

        list_for_each_entry(struct gstplayer_meta, meta, &plugin.players, entry) {
            *texture_ids_cursor++ = gstplayer_get_texture_id(meta->player);
        }

        plugin_unlock(&plugin);

        platch_respond_illegal_arg_ext_std(
            responsehandle,
            error_message,
            &STDMAP2(
                STDSTRING("receivedTextureId"),
                STDINT64(texture_id),
                STDSTRING("registeredTextureIds"),
                ((struct std_value){ .type = kStdInt64Array, .size = n_texture_ids, .int64array = texture_ids })
            )
        );

        return NULL;
    }

    return player;
}

static struct gstplayer *
get_player_from_v2_root_arg(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    int64_t texture_id;

    if (!raw_std_value_is_int(arg)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg` to be an integer.");
        return NULL;
    }

    texture_id = raw_std_value_as_int(arg);

    return get_player_from_texture_id_with_custom_errmsg(texture_id, responsehandle, "Expected `arg` to be a valid texture id.");
}

static struct gstplayer *
get_player_from_v2_list_arg(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    int64_t texture_id;

    if (!(raw_std_value_is_list(arg) && raw_std_list_get_size(arg) >= 1)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg` to be a list with at least one element.");
        return NULL;
    }

    const struct raw_std_value *first_element = raw_std_list_get_first_element(arg);
    if (!raw_std_value_is_int(first_element)) {
        platch_respond_illegal_arg_std(responsehandle, "Expected `arg[0]` to be an integer.");
        return NULL;
    }

    texture_id = raw_std_value_as_int(first_element);

    return get_player_from_texture_id_with_custom_errmsg(texture_id, responsehandle, "Expected `arg[0]` to be a valid texture id.");
}

static int
check_arg_is_minimum_sized_list(const struct raw_std_value *arg, size_t minimum_size, FlutterPlatformMessageResponseHandle *responsehandle) {
    int ok;

    if (!(raw_std_value_is_list(arg) && raw_std_list_get_size(arg) >= minimum_size)) {
        char *error_message = NULL;

        ok = asprintf(&error_message, "Expected `arg` to be a list with at least %zu element(s).", minimum_size);
        if (ok < 0) {
            platch_respond_illegal_arg_std(responsehandle, "Expected `arg` to be a list with a minimum size.");
            return EINVAL;
        }

        platch_respond_illegal_arg_std(responsehandle, error_message);

        free(error_message);

        return EINVAL;
    }

    return 0;
}

static int on_initialize_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    int ok;

    (void) arg;

    ok = ensure_initialized();
    if (ok != 0) {
        return respond_init_failed(responsehandle);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_create_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *headers;
    struct gstplayer_meta *meta;
    struct gstplayer *player;
    enum format_hint format_hint;
    char *asset, *uri, *package_name, *pipeline;
    char *webview_init_script, *webview_init_script_allow_origin;
    size_t size;
    int ok;

    ok = ensure_initialized();
    if (ok != 0) {
        return respond_init_failed_v2(responsehandle);
    }

    if (!raw_std_value_is_list(arg)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg` to be a List.");
    }

    size = raw_std_list_get_size(arg);

    // arg[0]: Asset Path
    if (size >= 1) {
        arg = raw_std_list_get_first_element(arg);

        if (raw_std_value_is_null(arg)) {
            asset = NULL;
        } else if (raw_std_value_is_string(arg)) {
            asset = raw_std_string_dup(arg);
            if (asset == NULL) {
                ok = ENOMEM;
                goto fail_respond_error;
            }
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[0]` to be a String or null.");
        }
    } else {
        asset = NULL;
    }

    // arg[1]: Package Name
    if (size >= 2) {
        arg = raw_std_value_after(arg);

        if (raw_std_value_is_null(arg)) {
            package_name = NULL;
        } else if (raw_std_value_is_string(arg)) {
            package_name = raw_std_string_dup(arg);
            if (package_name == NULL) {
                ok = ENOMEM;
                goto fail_respond_error;
            }
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[1]` to be a String or null.");
        }
    } else {
        package_name = NULL;
    }

    // arg[1]: URI
    if (size >= 3) {
        arg = raw_std_value_after(arg);

        if (raw_std_value_is_null(arg)) {
            uri = NULL;
        } else if (raw_std_value_is_string(arg)) {
            uri = raw_std_string_dup(arg);
            if (uri == NULL) {
                ok = ENOMEM;
                goto fail_respond_error;
            }
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[2]` to be a String or null.");
        }
    } else {
        uri = NULL;
    }

    // arg[3]: Format Hint
    if (size >= 4) {
        arg = raw_std_value_after(arg);

        if (raw_std_value_is_null(arg)) {
            format_hint = FORMAT_HINT_NONE;
        } else if (raw_std_value_is_string(arg)) {
            if (raw_std_string_equals(arg, "ss")) {
                format_hint = FORMAT_HINT_SS;
            } else if (raw_std_string_equals(arg, "hls")) {
                format_hint = FORMAT_HINT_HLS;
            } else if (raw_std_string_equals(arg, "dash")) {
                format_hint = FORMAT_HINT_MPEG_DASH;
            } else if (raw_std_string_equals(arg, "other")) {
                format_hint = FORMAT_HINT_OTHER;
            } else {
                goto invalid_format_hint;
            }
        } else {
invalid_format_hint:
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[3]` to be one of 'ss', 'hls', 'dash', 'other' or null.");
        }
    } else {
        format_hint = FORMAT_HINT_NONE;
    }

    // arg[4]: HTTP Headers
    if (size >= 5) {
        arg = raw_std_value_after(arg);

        if (raw_std_value_is_null(arg)) {
            headers = NULL;
        } else if (raw_std_value_is_map(arg)) {
            for_each_entry_in_raw_std_map(key, value, arg) {
                if (!raw_std_value_is_string(key) || !raw_std_value_is_string(value)) {
                    goto invalid_headers;
                }
            }
            headers = arg;
        } else {
invalid_headers:
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[4]` to be a map of strings or null.");
        }
    } else {
        headers = NULL;
    }

    // arg[5]: Gstreamer Pipeline
    if (size >= 6) {
        arg = raw_std_value_after(arg);

        if (raw_std_value_is_null(arg)) {
            pipeline = NULL;
        } else if (raw_std_value_is_string(arg)) {
            pipeline = raw_std_string_dup(arg);
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[5]` to be a string or null.");
        }
    } else {
        pipeline = NULL;
    }

    // arg[6]: Webview document-start init script (optional; pipeline only)
    if (size >= 7) {
        arg = raw_std_value_after(arg);

        if (raw_std_value_is_null(arg)) {
            webview_init_script = NULL;
        } else if (raw_std_value_is_string(arg)) {
            webview_init_script = raw_std_string_dup(arg);
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[6]` to be a string or null.");
        }
    } else {
        webview_init_script = NULL;
    }

    // arg[7]: Webview init-script allowed-origin match pattern (optional)
    if (size >= 8) {
        arg = raw_std_value_after(arg);

        if (raw_std_value_is_null(arg)) {
            webview_init_script_allow_origin = NULL;
        } else if (raw_std_value_is_string(arg)) {
            webview_init_script_allow_origin = raw_std_string_dup(arg);
        } else {
            return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[7]` to be a string or null.");
        }
    } else {
        webview_init_script_allow_origin = NULL;
    }

    if ((asset ? 1 : 0) + (uri ? 1 : 0) + (pipeline ? 1 : 0) != 1) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected exactly one of `arg[0]`, `arg[2]` or `arg[5]` to be non-null.");
    }

    // Create our actual player (this doesn't initialize it)
    if (asset != NULL) {
        player = gstplayer_new_from_asset(flutterpi, asset, package_name, NULL);

        // gstplayer_new_from_network will construct a file:// URI out of the
        // asset path internally.
        free(asset);
        asset = NULL;
    } else if (uri != NULL) {
        player = gstplayer_new_from_network(flutterpi, uri, format_hint, NULL);

        // gstplayer_new_from_network will dup the uri internally.
        free(uri);
        uri = NULL;
    } else if (pipeline != NULL) {
        player = gstplayer_new_from_pipeline(flutterpi, pipeline, NULL);

        // gstplayer_new_from_network will dup the pipeline internally.
        free(pipeline);
        pipeline = NULL;
    } else {
        UNREACHABLE();
    }

    if (player == NULL) {
        LOG_ERROR("Couldn't create gstreamer video player.\n");
        ok = EIO;
        goto fail_respond_error;
    }

    // create a meta object so we can store the event channel name
    // of a player with it
    meta = create_meta(gstplayer_get_texture_id(player), player);
    if (meta == NULL) {
        ok = ENOMEM;
        goto fail_destroy_player;
    }

    gstplayer_set_userdata_locked(player, meta);

    // Add all the HTTP headers to gstplayer using gstplayer_put_http_header
    if (headers != NULL) {
        for_each_entry_in_raw_std_map(header_name, header_value, headers) {
            char *header_name_duped = raw_std_string_dup(header_name);
            char *header_value_duped = raw_std_string_dup(header_value);

            gstplayer_put_http_header(player, header_name_duped, header_value_duped);

            free(header_value_duped);
            free(header_name_duped);
        }
    }

    // Apply the optional webview document-start init script. The setter dups
    // internally, so the local copies are freed right after. No-op for
    // non-webview players (they have no `websrc` element).
    gstplayer_set_webview_init_script(player, webview_init_script, webview_init_script_allow_origin);
    if (webview_init_script != NULL) {
        free(webview_init_script);
        webview_init_script = NULL;
    }
    if (webview_init_script_allow_origin != NULL) {
        free(webview_init_script_allow_origin);
        webview_init_script_allow_origin = NULL;
    }

    // Add it to our player collection
    add_player(meta);

    // Set a receiver on the videoEvents event channel
    ok = plugin_registry_set_receiver(meta->event_channel_name, kStandardMethodCall, on_receive_evch);
    if (ok != 0) {
        goto fail_remove_player;
    }

    // Subscribe to the player's error notifier so we can forward bus errors
    // (wpesrc failures, network errors, WebProcess crashes) to the Dart side
    // via the global error event channel. Non-fatal if it fails — we just
    // lose error reporting for this player.
    meta->error_listener = notifier_listen(gstplayer_get_error_notifier(player), on_error_notify, NULL, meta);
    if (meta->error_listener == NULL) {
        LOG_ERROR("Couldn't subscribe to gstplayer error notifier.\n");
    }

    // Finally, start initializing
    ok = gstplayer_initialize(player);
    if (ok != 0) {
        goto fail_remove_receiver;
    }

    return platch_respond_success_std(responsehandle, &STDINT64(gstplayer_get_texture_id(player)));

fail_remove_receiver:
    if (meta->error_listener != NULL) {
        notifier_unlisten(gstplayer_get_error_notifier(player), meta->error_listener);
        meta->error_listener = NULL;
    }
    plugin_registry_remove_receiver(meta->event_channel_name);

fail_remove_player:
    remove_player(meta);
    destroy_meta(meta);

fail_destroy_player:
    gstplayer_destroy(player);

fail_respond_error:
    return platch_respond_native_error_std(responsehandle, ok);
}

static int on_dispose_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;

    player = get_player_from_v2_root_arg(arg, responsehandle);
    if (player == NULL) {
        return EINVAL;
    }

    dispose_player(player, false, false);

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_set_looping_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *second;
    struct gstplayer *player;
    bool looping;
    int ok;

    ok = check_arg_is_minimum_sized_list(arg, 2, responsehandle);
    if (ok != 0) {
        return 0;
    }

    player = get_player_from_v2_list_arg(arg, responsehandle);
    if (player == NULL) {
        return 0;
    }

    second = raw_std_list_get_nth_element(arg, 1);
    if (raw_std_value_is_bool(second)) {
        looping = raw_std_value_as_bool(second);
    } else {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[1]` to be a bool.");
    }

    ok = gstplayer_set_looping(player, looping);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_set_volume_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *second;
    struct gstplayer *player;
    double volume;
    int ok;

    ok = check_arg_is_minimum_sized_list(arg, 2, responsehandle);
    if (ok != 0) {
        return 0;
    }

    player = get_player_from_v2_list_arg(arg, responsehandle);
    if (player == NULL) {
        return 0;
    }

    second = raw_std_list_get_nth_element(arg, 1);
    if (raw_std_value_is_float64(second)) {
        volume = raw_std_value_as_float64(second);
    } else {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[1]` to be a double.");
    }

    ok = gstplayer_set_volume(player, volume);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_set_playback_speed_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *second;
    struct gstplayer *player;
    double speed;
    int ok;

    ok = check_arg_is_minimum_sized_list(arg, 2, responsehandle);
    if (ok != 0) {
        return 0;
    }

    player = get_player_from_v2_list_arg(arg, responsehandle);
    if (player == NULL) {
        return 0;
    }

    second = raw_std_list_get_nth_element(arg, 1);
    if (raw_std_value_is_float64(second)) {
        speed = raw_std_value_as_float64(second);
    } else {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[1]` to be a double.");
    }

    ok = gstplayer_set_playback_speed(player, speed);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_play_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    int ok;

    player = get_player_from_v2_root_arg(arg, responsehandle);
    if (player == NULL) {
        return EINVAL;
    }

    ok = gstplayer_play(player);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_get_position_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    int64_t position;

    player = get_player_from_v2_root_arg(arg, responsehandle);
    if (player == NULL) {
        return EINVAL;
    }

    position = gstplayer_get_position(player);
    if (position < 0) {
        return platch_respond_native_error_std(responsehandle, EIO);
    }

    return platch_respond_success_std(responsehandle, &STDINT64(position));
}

static int on_seek_to_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *second;
    struct gstplayer *player;
    int64_t position;
    int ok;

    ok = check_arg_is_minimum_sized_list(arg, 2, responsehandle);
    if (ok != 0) {
        return 0;
    }

    player = get_player_from_v2_list_arg(arg, responsehandle);
    if (player == NULL) {
        return 0;
    }

    second = raw_std_list_get_nth_element(arg, 1);
    if (raw_std_value_is_int(second)) {
        position = raw_std_value_as_int(second);
    } else {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[1]` to be an integer.");
    }

    ok = gstplayer_seek_to(player, position, false);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_pause_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    int ok;

    player = get_player_from_v2_root_arg(arg, responsehandle);
    if (player == NULL) {
        return EINVAL;
    }

    ok = gstplayer_pause(player);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_set_mix_with_others_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) arg;
    (void) responsehandle;

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_fast_seek_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *second;
    struct gstplayer *player;
    int64_t position;
    int ok;

    ok = check_arg_is_minimum_sized_list(arg, 2, responsehandle);
    if (ok != 0) {
        return 0;
    }

    player = get_player_from_v2_list_arg(arg, responsehandle);
    if (player == NULL) {
        return 0;
    }

    second = raw_std_list_get_nth_element(arg, 1);
    if (raw_std_value_is_int(second)) {
        position = raw_std_value_as_int(second);
    } else {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[1]` to be an integer.");
    }

    ok = gstplayer_seek_to(player, position, true);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_step_forward_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    int ok;

    player = get_player_from_v2_root_arg(arg, responsehandle);
    if (player == NULL) {
        return EINVAL;
    }

    ok = gstplayer_step_forward(player);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_step_backward_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    int ok;

    player = get_player_from_v2_root_arg(arg, responsehandle);
    if (player == NULL) {
        return EINVAL;
    }

    ok = gstplayer_step_backward(player);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

/// Helper: read a double from a raw_std map entry, accepting both float64 and
/// integer values. Returns true on success.
static bool raw_std_map_get_double(const struct raw_std_value *map, const char *key, double *out) {
    const struct raw_std_value *v = raw_std_map_find_str(map, key);
    if (v == NULL) {
        return false;
    }
    if (raw_std_value_is_float64(v)) {
        *out = raw_std_value_as_float64(v);
        return true;
    }
    if (raw_std_value_is_int(v)) {
        *out = (double) raw_std_value_as_int(v);
        return true;
    }
    return false;
}

/// Handler for `sendNavigationEvent`. Argument is a map:
///   { textureId: int, type: string, button?: int, x: double, y: double,
///     deltaX?: double, deltaY?: double }
/// where `type` is one of "mouse-button-press", "mouse-button-release",
/// "mouse-scroll". The constructed GstNavigation event is sent upstream into
/// the player's pipeline (toward wpesrc / video source).
static int on_send_navigation_event_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct gstplayer *player;
    const struct raw_std_value *texture_id_v, *type_v, *button_v;
    int64_t texture_id;
    int button;
    double x, y, delta_x, delta_y;
    GstEvent *event;
    int ok;

    if (!raw_std_value_is_map(arg)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg` to be a Map.");
    }

    texture_id_v = raw_std_map_find_str(arg, "textureId");
    if (texture_id_v == NULL || !raw_std_value_is_int(texture_id_v)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['textureId']` to be an integer.");
    }
    texture_id = raw_std_value_as_int(texture_id_v);

    player = get_player_from_texture_id_with_custom_errmsg(texture_id, responsehandle, "Expected `arg['textureId']` to be a valid texture id.");
    if (player == NULL) {
        return 0;
    }

    type_v = raw_std_map_find_str(arg, "type");
    if (type_v == NULL || !raw_std_value_is_string(type_v)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['type']` to be a string.");
    }

    if (!raw_std_map_get_double(arg, "x", &x) || !raw_std_map_get_double(arg, "y", &y)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['x']` and `arg['y']` to be numbers.");
    }

    if (raw_std_string_equals(type_v, "mouse-button-press") || raw_std_string_equals(type_v, "mouse-button-release")) {
        button_v = raw_std_map_find_str(arg, "button");
        if (button_v != NULL && raw_std_value_is_int(button_v)) {
            button = (int) raw_std_value_as_int(button_v);
        } else {
            // Default to left mouse button when not specified.
            button = 1;
        }

        if (raw_std_string_equals(type_v, "mouse-button-press")) {
            event = gst_navigation_event_new_mouse_button_press(button, x, y, GST_NAVIGATION_MODIFIER_NONE);
        } else {
            event = gst_navigation_event_new_mouse_button_release(button, x, y, GST_NAVIGATION_MODIFIER_NONE);
        }
    } else if (raw_std_string_equals(type_v, "mouse-scroll")) {
        if (!raw_std_map_get_double(arg, "deltaX", &delta_x)) {
            delta_x = 0.0;
        }
        if (!raw_std_map_get_double(arg, "deltaY", &delta_y)) {
            delta_y = 0.0;
        }
        event = gst_navigation_event_new_mouse_scroll(x, y, delta_x, delta_y, GST_NAVIGATION_MODIFIER_NONE);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['type']` to be one of 'mouse-button-press', 'mouse-button-release', 'mouse-scroll'."
        );
    }

    if (event == NULL) {
        return platch_respond_native_error_std(responsehandle, ENOMEM);
    }

    ok = gstplayer_send_event(player, (struct _GstEvent *) event);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

/// Handler for `setPipelineState`. Argument is `[textureId, state]`, where
/// `state` is a string ("PLAYING" or "PAUSED"). Bypasses the gstplayer
/// playpause state machine and pokes the pipeline directly — useful for
/// webview idle-suspend.
static int on_set_pipeline_state_v2(const struct raw_std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *second;
    struct gstplayer *player;
    char *state;
    int ok;

    ok = check_arg_is_minimum_sized_list(arg, 2, responsehandle);
    if (ok != 0) {
        return 0;
    }

    player = get_player_from_v2_list_arg(arg, responsehandle);
    if (player == NULL) {
        return 0;
    }

    second = raw_std_list_get_nth_element(arg, 1);
    if (!raw_std_value_is_string(second)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg[1]` to be a string.");
    }

    state = raw_std_string_dup(second);
    if (state == NULL) {
        return platch_respond_native_error_std(responsehandle, ENOMEM);
    }

    ok = gstplayer_set_pipeline_state(player, state);
    free(state);

    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_receive_method_channel_v2(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    const struct raw_std_value *envelope, *method, *arg;

    ASSERT_NOT_NULL(channel);
    ASSERT_NOT_NULL(object);
    ASSERT_NOT_NULL(responsehandle);
    ASSERT_EQUALS(object->codec, kBinaryCodec);
    assert(object->binarydata_size != 0);
    (void) channel;

    envelope = (const struct raw_std_value *) (object->binarydata);

    if (!raw_std_method_call_check(envelope, object->binarydata_size)) {
        return platch_respond_error_std(responsehandle, "malformed-message", "", &STDNULL);
    }

    method = raw_std_method_call_get_method(envelope);
    arg = raw_std_method_call_get_arg(envelope);

    if (raw_std_string_equals(method, "initialize")) {
        return on_initialize_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "create")) {
        return on_create_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "dispose")) {
        return on_dispose_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "setLooping")) {
        return on_set_looping_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "setVolume")) {
        return on_set_volume_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "setPlaybackSpeed")) {
        return on_set_playback_speed_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "play")) {
        return on_play_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "getPosition")) {
        return on_get_position_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "seekTo")) {
        return on_seek_to_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "pause")) {
        return on_pause_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "setMixWithOthers")) {
        return on_set_mix_with_others_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "stepForward")) {
        return on_step_forward_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "stepBackward")) {
        return on_step_backward_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "fastSeek")) {
        return on_fast_seek_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "sendNavigationEvent")) {
        return on_send_navigation_event_v2(arg, responsehandle);
    } else if (raw_std_string_equals(method, "setPipelineState")) {
        return on_set_pipeline_state_v2(arg, responsehandle);
    } else {
        return platch_respond_not_implemented(responsehandle);
    }
}

enum plugin_init_result gstplayer_plugin_init(struct flutterpi *flutterpi, void **userdata_out) {
    int ok;

    (void) userdata_out;

    plugin.flutterpi = flutterpi;
    plugin.initialized = false;
    plugin.has_error_listener = false;

    ok = pthread_mutex_init(&plugin.lock, get_default_mutex_attrs());
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    list_inithead(&plugin.players);

    ok = plugin_registry_set_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.initialize", kStandardMessageCodec, on_initialize);
    if (ok != 0) {
        goto fail_destroy_lock;
    }

    ok = plugin_registry_set_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.create", kStandardMessageCodec, on_create);
    if (ok != 0) {
        goto fail_remove_initialize_receiver;
    }

    ok = plugin_registry_set_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.dispose", kStandardMessageCodec, on_dispose);
    if (ok != 0) {
        goto fail_remove_create_receiver;
    }

    ok = plugin_registry_set_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setLooping", kStandardMessageCodec, on_set_looping);
    if (ok != 0) {
        goto fail_remove_dispose_receiver;
    }

    ok = plugin_registry_set_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setVolume", kStandardMessageCodec, on_set_volume);
    if (ok != 0) {
        goto fail_remove_setLooping_receiver;
    }

    ok = plugin_registry_set_receiver_locked(
        "dev.flutter.pigeon.VideoPlayerApi.setPlaybackSpeed",
        kStandardMessageCodec,
        on_set_playback_speed
    );
    if (ok != 0) {
        goto fail_remove_setVolume_receiver;
    }

    ok = plugin_registry_set_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.play", kStandardMessageCodec, on_play);
    if (ok != 0) {
        goto fail_remove_setPlaybackSpeed_receiver;
    }

    ok = plugin_registry_set_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.position", kStandardMessageCodec, on_get_position);
    if (ok != 0) {
        goto fail_remove_play_receiver;
    }

    ok = plugin_registry_set_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.seekTo", kStandardMessageCodec, on_seek_to);
    if (ok != 0) {
        goto fail_remove_position_receiver;
    }

    ok = plugin_registry_set_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.pause", kStandardMessageCodec, on_pause);
    if (ok != 0) {
        goto fail_remove_seekTo_receiver;
    }

    ok = plugin_registry_set_receiver_locked(
        "dev.flutter.pigeon.VideoPlayerApi.setMixWithOthers",
        kStandardMessageCodec,
        on_set_mix_with_others
    );
    if (ok != 0) {
        goto fail_remove_pause_receiver;
    }

    ok = plugin_registry_set_receiver_locked(
        "flutter.io/videoPlayer/gstreamerVideoPlayer/advancedControls",
        kStandardMethodCall,
        on_receive_method_channel
    );
    if (ok != 0) {
        goto fail_remove_setMixWithOthers_receiver;
    }

    ok = plugin_registry_set_receiver_locked("flutter-pi/gstreamerVideoPlayer", kBinaryCodec, on_receive_method_channel_v2);
    if (ok != 0) {
        goto fail_remove_advancedControls_receiver;
    }

    ok = plugin_registry_set_receiver_locked(ERROR_EVENT_CHANNEL, kStandardMethodCall, on_receive_error_evch);
    if (ok != 0) {
        goto fail_remove_method_channel_v2_receiver;
    }

    return PLUGIN_INIT_RESULT_INITIALIZED;

fail_remove_method_channel_v2_receiver:
    plugin_registry_remove_receiver_locked("flutter-pi/gstreamerVideoPlayer");

fail_remove_advancedControls_receiver:
    plugin_registry_remove_receiver_locked("flutter.io/videoPlayer/gstreamerVideoPlayer/advancedControls");

fail_remove_setMixWithOthers_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setMixWithOthers");

fail_remove_pause_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.pause");

fail_remove_seekTo_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.seekTo");

fail_remove_position_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.position");

fail_remove_play_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.play");

fail_remove_setPlaybackSpeed_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setPlaybackSpeed");

fail_remove_setVolume_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setVolume");

fail_remove_setLooping_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setLooping");

fail_remove_dispose_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.dispose");

fail_remove_create_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.create");

fail_remove_initialize_receiver:
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.initialize");

fail_destroy_lock:
    pthread_mutex_destroy(&plugin.lock);
    return PLUGIN_INIT_RESULT_ERROR;
}

void gstplayer_plugin_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    (void) userdata;

    plugin_lock(&plugin);

    list_for_each_entry_safe(struct gstplayer_meta, meta, &plugin.players, entry) {
        // will also remove the player from the player list.
        dispose_player(meta->player, true, true);
    }

    if (plugin.initialized) {
        gst_deinit();
        plugin.initialized = false;
    }

    plugin_unlock(&plugin);

    plugin_registry_remove_receiver_locked(ERROR_EVENT_CHANNEL);
    plugin_registry_remove_receiver_locked("flutter-pi/gstreamerVideoPlayer");
    plugin_registry_remove_receiver_locked("flutter.io/videoPlayer/gstreamerVideoPlayer/advancedControls");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setMixWithOthers");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.pause");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.seekTo");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.position");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.play");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setPlaybackSpeed");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setVolume");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.setLooping");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.dispose");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.create");
    plugin_registry_remove_receiver_locked("dev.flutter.pigeon.VideoPlayerApi.initialize");
    pthread_mutex_destroy(&plugin.lock);
}

FLUTTERPI_PLUGIN("gstreamer video_player", gstplayer, gstplayer_plugin_init, gstplayer_plugin_deinit)
