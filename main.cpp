#include <furi.h>

#include <gui/gui.h>
#include <gui/modules/file_browser.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <storage/storage.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/ATMlib.h"
#include "atm_icons.h"

#define ATM_TXT_MAGIC        "ATM1"
#define ATM_TXT_CMD_ENTRY    "ENTRY"
#define ATM_TXT_CMD_TRACK    "TRACK"
#define ATM_TXT_CMD_ENDTRACK "ENDTRACK"
#define ATM_TXT_CMD_END      "END"
#define ATM_TXT_COMMENT      '#'
#define ATM_TXT_SEPARATOR    ','

#define ATM_TXT_OP_DB                "DB"
#define ATM_TXT_OP_NOTE              "NOTE"
#define ATM_TXT_OP_DELAY             "DELAY"
#define ATM_TXT_OP_STOP              "STOP"
#define ATM_TXT_OP_RETURN            "RETURN"
#define ATM_TXT_OP_GOTO              "GOTO"
#define ATM_TXT_OP_REPEAT            "REPEAT"
#define ATM_TXT_OP_SET_TEMPO         "SET_TEMPO"
#define ATM_TXT_OP_ADD_TEMPO         "ADD_TEMPO"
#define ATM_TXT_OP_SET_VOLUME        "SET_VOLUME"
#define ATM_TXT_OP_VOLUME_SLIDE_ON   "VOLUME_SLIDE_ON"
#define ATM_TXT_OP_VOLUME_SLIDE_OFF  "VOLUME_SLIDE_OFF"
#define ATM_TXT_OP_SET_NOTE_CUT      "SET_NOTE_CUT"
#define ATM_TXT_OP_NOTE_CUT_OFF      "NOTE_CUT_OFF"
#define ATM_TXT_OP_SET_TRANSPOSITION "SET_TRANSPOSITION"
#define ATM_TXT_OP_TRANSPOSITION_OFF "TRANSPOSITION_OFF"
#define ATM_TXT_OP_GOTO_ADVANCED     "GOTO_ADVANCED"
#define ATM_TXT_OP_SET_VIBRATO       "SET_VIBRATO"

#define ATM_SONG_MAX_TEXT_SIZE (32 * 1024)

typedef enum {
    AtmViewBrowser = 0,
    AtmViewPlayer,
} AtmView;

typedef enum {
    AtmEventFileSelected = 1,
    AtmEventOpenBrowser,
} AtmEvent;

typedef struct {
    char file_name[48];
    char state_line[24];
    bool loaded;
} AtmPlayerModel;

typedef struct {
    const char* cur;
} AtmTokenizer;

typedef struct {
    uint8_t* bytes;
    size_t size;
    size_t capacity;
} ByteBuffer;

typedef struct {
    uint16_t* items;
    size_t size;
    size_t capacity;
} OffsetBuffer;

typedef struct {
    Gui* gui;
    Storage* storage;
    ViewDispatcher* dispatcher;
    FileBrowser* file_browser;
    View* player_view;
    FuriString* selected_path;

    bool browser_started;
    AtmView current_view;

    uint8_t* song_buf;
    size_t song_size;
    bool playing;
    bool paused;
} FlipperAtmApp;

static void atm_extract_file_name(const char* path, char* out, size_t out_size);
static bool atm_file_browser_item_callback(
    FuriString* path,
    void* context,
    uint8_t** icon,
    FuriString* item_name);

static bool atm_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static char atm_char_upper(char c) {
    if(c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static bool atm_token_equals(const char* token, const char* keyword) {
    while(*token && *keyword) {
        if(atm_char_upper(*token) != atm_char_upper(*keyword)) return false;
        token++;
        keyword++;
    }
    return (*token == '\0') && (*keyword == '\0');
}

static void atm_set_player_status(
    FlipperAtmApp* app,
    const char* file_name,
    const char* state,
    bool loaded) {
    with_view_model_cpp(
        app->player_view,
        AtmPlayerModel*,
        model,
        {
            snprintf(
                model->file_name, sizeof(model->file_name), "%s", file_name ? file_name : "-");
            snprintf(model->state_line, sizeof(model->state_line), "%s", state ? state : "-");
            model->loaded = loaded;
        },
        true);
}

static void atm_set_playback_state(FlipperAtmApp* app) {
    char short_name[48];
    atm_extract_file_name(
        furi_string_get_cstr(app->selected_path), short_name, sizeof(short_name));

    const char* state = "Stopped";
    if(app->playing) state = app->paused ? "Paused" : "Playing";

    atm_set_player_status(app, short_name, state, app->song_buf != NULL);
}

static void atm_extract_file_name(const char* path, char* out, size_t out_size) {
    const char* file = strrchr(path, '/');
    file = file ? (file + 1) : path;

    snprintf(out, out_size, "%s", file);

    size_t len = strlen(out);
    if(len > 4 && out[len - 4] == '.' && atm_char_upper(out[len - 3]) == 'A' &&
       atm_char_upper(out[len - 2]) == 'T' && atm_char_upper(out[len - 1]) == 'M') {
        out[len - 4] = '\0';
    }
}

static bool byte_buffer_push(ByteBuffer* b, uint8_t value) {
    if(b->size == b->capacity) {
        size_t next = (b->capacity == 0) ? 128 : (b->capacity * 2);
        uint8_t* n = (uint8_t*)realloc(b->bytes, next);
        if(!n) return false;
        b->bytes = n;
        b->capacity = next;
    }
    b->bytes[b->size++] = value;
    return true;
}

static bool byte_buffer_push_u8_from_i32(ByteBuffer* b, int32_t value) {
    return byte_buffer_push(b, (uint8_t)(value & 0xFF));
}

static bool byte_buffer_push_vle(ByteBuffer* b, uint32_t value) {
    uint8_t groups[5];
    size_t n = 0;

    do {
        groups[n++] = (uint8_t)(value & 0x7F);
        value >>= 7;
    } while(value && n < sizeof(groups));

    for(size_t i = n; i > 0; i--) {
        uint8_t out = groups[i - 1];
        if(i != 1) out |= 0x80;
        if(!byte_buffer_push(b, out)) return false;
    }

    return true;
}

static bool offset_buffer_push(OffsetBuffer* b, uint16_t value) {
    if(b->size == b->capacity) {
        size_t next = (b->capacity == 0) ? 16 : (b->capacity * 2);
        uint16_t* n = (uint16_t*)realloc(b->items, next * sizeof(uint16_t));
        if(!n) return false;
        b->items = n;
        b->capacity = next;
    }
    b->items[b->size++] = value;
    return true;
}

static bool atm_next_token(AtmTokenizer* tz, char* token, size_t token_size) {
    const char* p = tz->cur;

    while(*p) {
        if(*p == ATM_TXT_COMMENT) {
            while(*p && *p != '\n')
                p++;
            continue;
        }

        if(atm_is_space(*p) || *p == ATM_TXT_SEPARATOR) {
            p++;
            continue;
        }

        break;
    }

    if(!*p) {
        tz->cur = p;
        return false;
    }

    size_t n = 0;
    while(*p && !atm_is_space(*p) && (*p != ATM_TXT_SEPARATOR) && (*p != ATM_TXT_COMMENT)) {
        if((n + 1) < token_size) token[n++] = *p;
        p++;
    }

    token[n] = '\0';
    tz->cur = p;
    return n > 0;
}

static bool atm_parse_i32(const char* token, int32_t* out) {
    char* end = NULL;
    long value = strtol(token, &end, 0);
    if(!end || (*end != '\0')) return false;
    *out = (int32_t)value;
    return true;
}

static bool atm_parse_arg_i32(AtmTokenizer* tz, int32_t* out) {
    char token[32];
    if(!atm_next_token(tz, token, sizeof(token))) return false;
    return atm_parse_i32(token, out);
}

static bool atm_emit_instruction(AtmTokenizer* tz, const char* op, ByteBuffer* data) {
    int32_t a = 0;
    int32_t b = 0;
    int32_t c = 0;
    int32_t d = 0;

    if(atm_token_equals(op, ATM_TXT_OP_DB)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        return byte_buffer_push_u8_from_i32(data, a);
    }

    if(atm_token_equals(op, ATM_TXT_OP_NOTE)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        if(a < 0 || a > 63) return false;
        return byte_buffer_push_u8_from_i32(data, a);
    }

    if(atm_token_equals(op, ATM_TXT_OP_DELAY)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        if(a < 1) return false;

        if(a <= 64) {
            return byte_buffer_push_u8_from_i32(data, 159 + a);
        } else {
            if(!byte_buffer_push(data, 224)) return false;
            return byte_buffer_push_vle(data, (uint32_t)(a - 65));
        }
    }

    if(atm_token_equals(op, ATM_TXT_OP_STOP)) {
        return byte_buffer_push(data, 0x9F);
    }

    if(atm_token_equals(op, ATM_TXT_OP_RETURN)) {
        return byte_buffer_push(data, 0xFE);
    }

    if(atm_token_equals(op, ATM_TXT_OP_GOTO)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        return byte_buffer_push(data, 0xFC) && byte_buffer_push_u8_from_i32(data, a);
    }

    if(atm_token_equals(op, ATM_TXT_OP_REPEAT)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        if(!atm_parse_arg_i32(tz, &b)) return false;
        return byte_buffer_push(data, 0xFD) && byte_buffer_push_u8_from_i32(data, a) &&
               byte_buffer_push_u8_from_i32(data, b);
    }

    if(atm_token_equals(op, ATM_TXT_OP_SET_TEMPO)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        return byte_buffer_push(data, 0x9D) && byte_buffer_push_u8_from_i32(data, a);
    }

    if(atm_token_equals(op, ATM_TXT_OP_ADD_TEMPO)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        return byte_buffer_push(data, 0x9C) && byte_buffer_push_u8_from_i32(data, a);
    }

    if(atm_token_equals(op, ATM_TXT_OP_SET_VOLUME)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        return byte_buffer_push(data, 0x40) && byte_buffer_push_u8_from_i32(data, a);
    }

    if(atm_token_equals(op, ATM_TXT_OP_VOLUME_SLIDE_ON)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        return byte_buffer_push(data, 0x41) && byte_buffer_push_u8_from_i32(data, a);
    }

    if(atm_token_equals(op, ATM_TXT_OP_VOLUME_SLIDE_OFF)) {
        return byte_buffer_push(data, 0x43);
    }

    if(atm_token_equals(op, ATM_TXT_OP_SET_NOTE_CUT)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        return byte_buffer_push(data, 0x54) && byte_buffer_push_u8_from_i32(data, a);
    }

    if(atm_token_equals(op, ATM_TXT_OP_NOTE_CUT_OFF)) {
        return byte_buffer_push(data, 0x55);
    }

    if(atm_token_equals(op, ATM_TXT_OP_SET_TRANSPOSITION)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        return byte_buffer_push(data, 0x4C) && byte_buffer_push_u8_from_i32(data, a);
    }

    if(atm_token_equals(op, ATM_TXT_OP_TRANSPOSITION_OFF)) {
        return byte_buffer_push(data, 0x4D);
    }

    if(atm_token_equals(op, ATM_TXT_OP_GOTO_ADVANCED)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        if(!atm_parse_arg_i32(tz, &b)) return false;
        if(!atm_parse_arg_i32(tz, &c)) return false;
        if(!atm_parse_arg_i32(tz, &d)) return false;
        return byte_buffer_push(data, 0x9E) && byte_buffer_push_u8_from_i32(data, a) &&
               byte_buffer_push_u8_from_i32(data, b) && byte_buffer_push_u8_from_i32(data, c) &&
               byte_buffer_push_u8_from_i32(data, d);
    }

    if(atm_token_equals(op, ATM_TXT_OP_SET_VIBRATO)) {
        if(!atm_parse_arg_i32(tz, &a)) return false;
        if(!atm_parse_arg_i32(tz, &b)) return false;
        return byte_buffer_push(data, 0x4E) && byte_buffer_push_u8_from_i32(data, a) &&
               byte_buffer_push_u8_from_i32(data, b);
    }

    if(atm_parse_i32(op, &a)) {
        return byte_buffer_push_u8_from_i32(data, a);
    }

    return false;
}

static bool atm_parse_song_text(const char* text, uint8_t** out_buf, size_t* out_size) {
    AtmTokenizer tz = {.cur = text};
    char token[64];

    uint8_t entry[4] = {0};
    int32_t value = 0;

    ByteBuffer data = {NULL, 0, 0};
    OffsetBuffer offsets = {NULL, 0, 0};

    uint8_t* song = NULL;
    size_t song_size = 0;
    size_t p = 0;
    bool ok = false;

    if(!atm_next_token(&tz, token, sizeof(token)) || !atm_token_equals(token, ATM_TXT_MAGIC))
        goto out;

    if(!atm_next_token(&tz, token, sizeof(token)) || !atm_token_equals(token, ATM_TXT_CMD_ENTRY))
        goto out;
    for(size_t i = 0; i < 4; i++) {
        if(!atm_parse_arg_i32(&tz, &value)) goto out;
        entry[i] = (uint8_t)(value & 0xFF);
    }

    while(atm_next_token(&tz, token, sizeof(token))) {
        if(atm_token_equals(token, ATM_TXT_CMD_END)) {
            break;
        }

        if(!atm_token_equals(token, ATM_TXT_CMD_TRACK)) goto out;

        if(!offset_buffer_push(&offsets, (uint16_t)data.size)) goto out;

        while(atm_next_token(&tz, token, sizeof(token))) {
            if(atm_token_equals(token, ATM_TXT_CMD_ENDTRACK)) break;
            if(!atm_emit_instruction(&tz, token, &data)) goto out;
        }

        if(!atm_token_equals(token, ATM_TXT_CMD_ENDTRACK)) goto out;
    }

    if(!atm_token_equals(token, ATM_TXT_CMD_END)) goto out;
    if(offsets.size == 0 || offsets.size > 255) goto out;

    song_size = 1 + offsets.size * 2 + 4 + data.size;
    song = (uint8_t*)malloc(song_size);
    if(!song) goto out;

    song[p++] = (uint8_t)offsets.size;
    for(size_t i = 0; i < offsets.size; i++) {
        uint16_t off = offsets.items[i];
        song[p++] = (uint8_t)(off & 0xFF);
        song[p++] = (uint8_t)((off >> 8) & 0xFF);
    }
    for(size_t i = 0; i < 4; i++) {
        song[p++] = entry[i];
    }
    memcpy(song + p, data.bytes, data.size);

    *out_buf = song;
    *out_size = song_size;
    song = NULL;
    ok = true;

out:
    if(song) free(song);
    if(data.bytes) free(data.bytes);
    if(offsets.items) free(offsets.items);
    return ok;
}

static bool atm_load_song_from_file(FlipperAtmApp* app, const char* path) {
    bool ok = false;
    File* file = storage_file_alloc(app->storage);
    if(!file) return false;

    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;

        uint64_t file_size = storage_file_size(file);
        if(file_size == 0 || file_size > ATM_SONG_MAX_TEXT_SIZE) break;

        char* text = (char*)malloc((size_t)file_size + 1);
        if(!text) break;

        size_t read_total = 0;
        while(read_total < (size_t)file_size) {
            size_t r = storage_file_read(file, text + read_total, (size_t)file_size - read_total);
            if(r == 0) break;
            read_total += r;
        }
        text[read_total] = '\0';

        uint8_t* compiled = NULL;
        size_t compiled_size = 0;
        if(read_total == (size_t)file_size &&
           atm_parse_song_text(text, &compiled, &compiled_size)) {
            if(app->song_buf) free(app->song_buf);
            app->song_buf = compiled;
            app->song_size = compiled_size;
            ok = true;
        }

        free(text);
    } while(false);

    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

static void atm_file_selected_callback(void* context) {
    FlipperAtmApp* app = (FlipperAtmApp*)context;
    view_dispatcher_send_custom_event(app->dispatcher, AtmEventFileSelected);
}

static void atm_open_browser(FlipperAtmApp* app) {
    if(!app->browser_started) {
        file_browser_start(app->file_browser, app->selected_path);
        app->browser_started = true;
    }

    app->current_view = AtmViewBrowser;
    view_dispatcher_switch_to_view(app->dispatcher, AtmViewBrowser);
}

static void atm_player_draw_callback(Canvas* canvas, void* model_ptr) {
    AtmPlayerModel* model = (AtmPlayerModel*)model_ptr;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "FlipperATM");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 24, "File:");
    canvas_draw_str(canvas, 30, 24, model->file_name);

    canvas_draw_str(canvas, 2, 36, "State:");
    canvas_draw_str(canvas, 38, 36, model->state_line);

    canvas_draw_str(canvas, 2, 49, "OK pause/resume");
    canvas_draw_str(canvas, 2, 58, "Down stop, Back files");
}

static bool atm_player_input_callback(InputEvent* event, void* context) {
    FlipperAtmApp* app = (FlipperAtmApp*)context;
    bool consumed = false;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyOk && app->song_buf) {
            if(!app->playing) {
                ATM.play(app->song_buf);
                app->playing = true;
                app->paused = false;
            } else {
                ATM.playPause();
                app->paused = !app->paused;
            }
            atm_set_playback_state(app);
            consumed = true;
        } else if(event->key == InputKeyDown) {
            ATM.stop();
            app->playing = false;
            app->paused = false;
            atm_set_playback_state(app);
            consumed = true;
        }
    }

    return consumed;
}

static bool atm_custom_event_callback(void* context, uint32_t event) {
    FlipperAtmApp* app = (FlipperAtmApp*)context;

    if(event == AtmEventOpenBrowser) {
        atm_open_browser(app);
        return true;
    }

    if(event == AtmEventFileSelected) {
        if(app->browser_started) {
            file_browser_stop(app->file_browser);
            app->browser_started = false;
        }

        char short_name[48];
        atm_extract_file_name(
            furi_string_get_cstr(app->selected_path), short_name, sizeof(short_name));

        if(atm_load_song_from_file(app, furi_string_get_cstr(app->selected_path))) {
            ATM.play(app->song_buf);
            app->playing = true;
            app->paused = false;
            atm_set_player_status(app, short_name, "Playing", true);
        } else {
            ATM.stop();
            app->playing = false;
            app->paused = false;
            atm_set_player_status(app, short_name, "Load error", false);
        }

        app->current_view = AtmViewPlayer;
        view_dispatcher_switch_to_view(app->dispatcher, AtmViewPlayer);
        return true;
    }

    return false;
}

static bool atm_navigation_event_callback(void* context) {
    FlipperAtmApp* app = (FlipperAtmApp*)context;

    if(app->current_view == AtmViewPlayer) {
        view_dispatcher_send_custom_event(app->dispatcher, AtmEventOpenBrowser);
        return true;
    }

    return false;
}

static bool atm_file_browser_item_callback(
    FuriString* path,
    void* context,
    uint8_t** icon,
    FuriString* item_name) {
    UNUSED(path);
    UNUSED(context);
    UNUSED(item_name);

    const Icon* file_icon = &I_icon;
    const uint8_t* frame = icon_get_frame_data(file_icon, 0);
    if(!frame || !(*icon)) return false;

    // file_browser custom icon buffer size is fixed at 32 bytes.
    memcpy(*icon, frame, 32);
    return true;
}

extern "C" int32_t flipper_atm_app(void* p) {
    UNUSED(p);

    FlipperAtmApp* app = (FlipperAtmApp*)malloc(sizeof(FlipperAtmApp));
    if(!app) return -1;
    memset(app, 0, sizeof(FlipperAtmApp));

    app->gui = (Gui*)furi_record_open(RECORD_GUI);
    app->storage = (Storage*)furi_record_open(RECORD_STORAGE);
    app->dispatcher = view_dispatcher_alloc();

    app->selected_path = furi_string_alloc();
    furi_string_set_str(app->selected_path, APP_ASSETS_PATH("title.atm"));

    app->file_browser = file_browser_alloc(app->selected_path);
    file_browser_configure(
        app->file_browser, ".atm", APP_ASSETS_PATH(""), false, true, NULL, true);
    file_browser_set_callback(app->file_browser, atm_file_selected_callback, app);
    file_browser_set_item_callback(app->file_browser, atm_file_browser_item_callback, app);

    app->player_view = view_alloc();
    view_set_context(app->player_view, app);
    view_allocate_model(app->player_view, ViewModelTypeLocking, sizeof(AtmPlayerModel));
    view_set_draw_callback(app->player_view, atm_player_draw_callback);
    view_set_input_callback(app->player_view, atm_player_input_callback);

    atm_set_player_status(app, "-", "Choose file", false);

    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->dispatcher, atm_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, atm_navigation_event_callback);

    view_dispatcher_add_view(
        app->dispatcher, AtmViewBrowser, file_browser_get_view(app->file_browser));
    view_dispatcher_add_view(app->dispatcher, AtmViewPlayer, app->player_view);
    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    atm_system_init();
    atm_set_enabled(1);

    atm_open_browser(app);
    view_dispatcher_run(app->dispatcher);

    ATM.stop();
    atm_system_deinit();

    if(app->browser_started) {
        file_browser_stop(app->file_browser);
    }

    if(app->song_buf) free(app->song_buf);

    view_dispatcher_remove_view(app->dispatcher, AtmViewBrowser);
    view_dispatcher_remove_view(app->dispatcher, AtmViewPlayer);
    file_browser_free(app->file_browser);
    view_free(app->player_view);

    view_dispatcher_free(app->dispatcher);
    furi_string_free(app->selected_path);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);

    free(app);
    return 0;
}
