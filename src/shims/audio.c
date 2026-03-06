#include "engine.h"
#include <string.h>
#include <unistd.h>

// Audio chunk storage keyed by integer ID
static GHashTable *audio_table = NULL;
static int next_audio_id = 1;

static void audio_chunk_free(gpointer data) {
    Mix_FreeChunk((Mix_Chunk *)data);
}

// __audioLoadChunkFromBuffer(arraybuffer) -> id or 0
// Writes the arraybuffer to a temp file and loads with SDL_mixer
static JSCValue *native_audio_load_from_buffer(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_number(ctx, 0);

    JSCValue *buf = g_ptr_array_index(args, 0);
    gsize len = 0;
    void *data = NULL;

    if (jsc_value_is_array_buffer(buf)) {
        data = jsc_value_array_buffer_get_data(buf, &len);
    } else if (jsc_value_is_typed_array(buf)) {
        data = jsc_value_typed_array_get_data(buf, &len);
        len = jsc_value_typed_array_get_size(buf);
    }

    if (!data || len == 0) return jsc_value_new_number(ctx, 0);

    // Write to temp file for SDL_mixer
    char tmppath[] = "/tmp/pq_audio_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return jsc_value_new_number(ctx, 0);
    write(fd, data, len);
    close(fd);

    Mix_Chunk *chunk = Mix_LoadWAV(tmppath);
    unlink(tmppath);

    if (!chunk) {
        fprintf(stderr, "[Audio] Failed to decode buffer (%zu bytes): %s\n", len, Mix_GetError());
        return jsc_value_new_number(ctx, 0);
    }

    int id = next_audio_id++;
    if (!audio_table) {
        audio_table = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, audio_chunk_free);
    }
    g_hash_table_insert(audio_table, GINT_TO_POINTER(id), chunk);
    printf("[Audio] Decoded buffer -> id=%d (%zu bytes)\n", id, len);

    return jsc_value_new_number(ctx, id);
}

// __audioLoadChunk(path) -> id or 0
static JSCValue *native_audio_load_chunk(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_number(ctx, 0);

    char *url = jsc_value_to_string(g_ptr_array_index(args, 0));
    char *path = engine_resolve_path(url);
    g_free(url);

    Mix_Chunk *chunk = Mix_LoadWAV(path);
    if (!chunk) {
        fprintf(stderr, "[Audio] Failed to load: %s (%s)\n", path, Mix_GetError());
        free(path);
        return jsc_value_new_number(ctx, 0);
    }

    int id = next_audio_id++;
    if (!audio_table) {
        audio_table = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, audio_chunk_free);
    }
    g_hash_table_insert(audio_table, GINT_TO_POINTER(id), chunk);
    printf("[Audio] Loaded: %s (id=%d)\n", path, id);
    free(path);

    return jsc_value_new_number(ctx, id);
}

// __audioPlayChunk(id, volume, loop) -> channel or -1
static JSCValue *native_audio_play_chunk(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_number(ctx, -1);
    int id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    double volume = args->len > 1 ? jsc_value_to_double(g_ptr_array_index(args, 1)) : 1.0;
    int loop = args->len > 2 ? jsc_value_to_int32(g_ptr_array_index(args, 2)) : 0;

    if (!audio_table) return jsc_value_new_number(ctx, -1);
    Mix_Chunk *chunk = g_hash_table_lookup(audio_table, GINT_TO_POINTER(id));
    if (!chunk) return jsc_value_new_number(ctx, -1);

    int vol = (int)(volume * MIX_MAX_VOLUME);
    Mix_VolumeChunk(chunk, vol);
    int channel = Mix_PlayChannel(-1, chunk, loop ? -1 : 0);
    return jsc_value_new_number(ctx, channel);
}

// __audioStopChannel(channel)
static void native_audio_stop_channel(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return;
    int channel = jsc_value_to_int32(g_ptr_array_index(args, 0));
    if (channel >= 0) Mix_HaltChannel(channel);
}

// __audioSetChannelVolume(channel, volume)
static void native_audio_set_volume(GPtrArray *args, gpointer user_data) {
    if (args->len < 2) return;
    int channel = jsc_value_to_int32(g_ptr_array_index(args, 0));
    double volume = jsc_value_to_double(g_ptr_array_index(args, 1));
    if (channel >= 0) Mix_Volume(channel, (int)(volume * MIX_MAX_VOLUME));
}

void register_audio_shim(JSCContext *ctx) {
    // Register native audio functions
    JSCValue *load_fn = jsc_value_new_function_variadic(ctx, "__audioLoadChunk",
        G_CALLBACK(native_audio_load_chunk), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "__audioLoadChunk", load_fn);
    g_object_unref(load_fn);

    JSCValue *play_fn = jsc_value_new_function_variadic(ctx, "__audioPlayChunk",
        G_CALLBACK(native_audio_play_chunk), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "__audioPlayChunk", play_fn);
    g_object_unref(play_fn);

    JSCValue *stop_fn = jsc_value_new_function_variadic(ctx, "__audioStopChannel",
        G_CALLBACK(native_audio_stop_channel), NULL, NULL, G_TYPE_NONE);
    jsc_context_set_value(ctx, "__audioStopChannel", stop_fn);
    g_object_unref(stop_fn);

    JSCValue *vol_fn = jsc_value_new_function_variadic(ctx, "__audioSetChannelVolume",
        G_CALLBACK(native_audio_set_volume), NULL, NULL, G_TYPE_NONE);
    jsc_context_set_value(ctx, "__audioSetChannelVolume", vol_fn);
    g_object_unref(vol_fn);

    JSCValue *buf_fn = jsc_value_new_function_variadic(ctx, "__audioLoadFromBuffer",
        G_CALLBACK(native_audio_load_from_buffer), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "__audioLoadFromBuffer", buf_fn);
    g_object_unref(buf_fn);

    // AudioContext with native-backed decodeAudioData and playback
    jsc_context_evaluate(ctx,
        "window.AudioContext = function() {"
        "  this.state = 'running';"
        "  this.currentTime = 0;"
        "  this.sampleRate = 44100;"
        "  this.destination = { _type: 'destination' };"
        "  this.createGain = function() {"
        "    var g = { value: 1,"
        "      setValueAtTime: function(v) { this.value = v; },"
        "      linearRampToValueAtTime: function(v) { this.value = v; },"
        "      exponentialRampToValueAtTime: function(v) { this.value = v; },"
        "      setTargetAtTime: function(v) { this.value = v; },"
        "      cancelScheduledValues: function() {}"
        "    };"
        "    return { gain: g, connect: function(){ return this; }, disconnect: function(){} };"
        "  };"
        "  this.createBufferSource = function() {"
        "    var src = {"
        "      buffer: null, loop: false, loopStart: 0, loopEnd: 0,"
        "      _volume: 1.0, _channel: -1,"
        "      playbackRate: { value: 1,"
        "        setValueAtTime: function(v) { this.value = v; },"
        "        linearRampToValueAtTime: function(v) { this.value = v; },"
        "        exponentialRampToValueAtTime: function(v) { this.value = v; },"
        "        setTargetAtTime: function(v) { this.value = v; },"
        "        cancelScheduledValues: function() {}"
        "      },"
        "      connect: function(dest) { if (dest && dest.gain) this._volume = dest.gain.value; return this; },"
        "      disconnect: function() {},"
        "      start: function() {"
        "        if (this.buffer && this.buffer._nativeId) {"
        "          this._channel = __audioPlayChunk(this.buffer._nativeId, this._volume, this.loop ? 1 : 0);"
        "        }"
        "      },"
        "      stop: function() {"
        "        if (this._channel >= 0) { __audioStopChannel(this._channel); this._channel = -1; }"
        "      },"
        "      addEventListener: function() {},"
        "      removeEventListener: function() {},"
        "      onended: null"
        "    };"
        "    return src;"
        "  };"
        "  this.decodeAudioData = function(buf, ok, err) {"
        "    var nativeId = 0;"
        "    if (buf) {"
        "      var u8 = new Uint8Array(buf.byteLength ? buf : (buf.buffer || buf));"
        "      nativeId = __audioLoadFromBuffer(u8);"
        "    }"
        "    var audioBuffer = {"
        "      duration: 1.0,"
        "      length: 44100,"
        "      numberOfChannels: 2,"
        "      sampleRate: 44100,"
        "      _nativeId: nativeId,"
        "      getChannelData: function() { return new Float32Array(44100); }"
        "    };"
        "    if (ok) setTimeout(function(){ ok(audioBuffer); }, 0);"
        "    return Promise.resolve(audioBuffer);"
        "  };"
        "  this.resume = function() { return Promise.resolve(); };"
        "  this.close = function() { return Promise.resolve(); };"
        "  this.createOscillator = function() {"
        "    return { frequency: { value: 440 }, connect: function(){}, start: function(){}, stop: function(){} };"
        "  };"
        "};"
        "window.webkitAudioContext = window.AudioContext;"
        , -1);
}
