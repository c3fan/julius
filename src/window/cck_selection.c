#include "cck_selection.h"

#include "core/dir.h"
#include "core/encoding.h"
#include "core/file.h"
#include "core/image_group.h"
#include "core/log.h"
#include "core/sha256.h"
#include "game/file.h"
#include "graphics/generic_button.h"
#include "graphics/graphics.h"
#include "graphics/image.h"
#include "graphics/image_button.h"
#include "graphics/lang_text.h"
#include "graphics/panel.h"
#include "graphics/scrollbar.h"
#include "graphics/text.h"
#include "graphics/window.h"
#include "input/input.h"
#include "platform/file_manager.h"
#include "scenario/criteria.h"
#include "scenario/invasion.h"
#include "scenario/map.h"
#include "scenario/property.h"
#include "sound/music.h"
#include "translation/translation.h"
#include "widget/scenario_minimap.h"
#include "window/city.h"
#include "window/plain_message_dialog.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#define MAX_SCENARIOS 15
#define MAX_CCK_MAPS 200
#define METADATA_LINE_MAX (2 * FILE_NAME_MAX + 8)
#define MAP_FILE_EXTENSION "map"
/* Local cache filename for the downloaded index */
#define JULIUS_LIST_CACHE "julius.list"
#ifdef HAVE_CURL
/* Remote campaign index URL and per-map base URL */
#define JULIUS_LIST_URL      "https://c3fan.github.io/c3-maps/julius.list"
#define JULIUS_MAP_BASE_URL  "https://c3fan.github.io/c3-maps/"
#define CURL_CONNECT_TIMEOUT_MS  2000L
#define CURL_LIST_TIMEOUT_MS     8000L
#define CURL_MAP_TIMEOUT_MS    120000L
#endif

typedef enum {
    CCK_SELECTION_CUSTOM = 0,
    CCK_SELECTION_NETWORK = 1,
} cck_selection_mode;

typedef struct {
    char filename[FILE_NAME_MAX];  /**< e.g. "Caesarea.map" */
    char name[FILE_NAME_MAX];      /**< display name without extension */
    char hash[SHA256_HEX_SIZE];    /**< expected SHA-256 (from remote list), or empty */
    int is_local;                  /**< 1 if the .map file is present on disk */
} cck_map_entry;

static void button_select_item(int index, int param2);
static void button_start_scenario(int param1, int param2);
static void button_download_map(int param1, int param2);
static void button_toggle_minimap(int param1, int param2);
static void on_scroll(void);

static image_button start_button =
    {600, 440, 27, 27, IB_NORMAL, GROUP_SIDEBAR_BUTTONS, 56, button_start_scenario, button_none, 1, 0, 1};

static generic_button toggle_minimap_button =
    {570, 87, 39, 28, button_toggle_minimap, button_none, 0, 0};

/* Download button: shown in network mode for remote maps not yet downloaded */
static generic_button download_button =
    {335, 460, 192, 22, button_download_map, button_none, 0, 0};

static generic_button file_buttons[] = {
    {18, 220, 252, 16, button_select_item, button_none, 0, 0},
    {18, 236, 252, 16, button_select_item, button_none, 1, 0},
    {18, 252, 252, 16, button_select_item, button_none, 2, 0},
    {18, 268, 252, 16, button_select_item, button_none, 3, 0},
    {18, 284, 252, 16, button_select_item, button_none, 4, 0},
    {18, 300, 252, 16, button_select_item, button_none, 5, 0},
    {18, 316, 252, 16, button_select_item, button_none, 6, 0},
    {18, 332, 252, 16, button_select_item, button_none, 7, 0},
    {18, 348, 252, 16, button_select_item, button_none, 8, 0},
    {18, 364, 252, 16, button_select_item, button_none, 9, 0},
    {18, 380, 252, 16, button_select_item, button_none, 10, 0},
    {18, 396, 252, 16, button_select_item, button_none, 11, 0},
    {18, 412, 252, 16, button_select_item, button_none, 12, 0},
    {18, 428, 252, 16, button_select_item, button_none, 13, 0},
    {18, 444, 252, 16, button_select_item, button_none, 14, 0},
};

static scrollbar_type scrollbar = {276, 210, 256, 260, MAX_SCENARIOS, on_scroll, 1, 8, 1};

static struct {
    int focus_button_id;
    int focus_toggle_button;
    int focus_download_button;
    int selected_item;
    int show_minimap;
    char selected_scenario_filename[FILE_NAME_MAX];
    uint8_t selected_scenario_display[FILE_NAME_MAX];
    cck_map_entry scenarios[MAX_CCK_MAPS];
    int num_scenarios;

    const dir_listing *fallback_scenarios;
    cck_selection_mode mode;
} data;

static char *skip_ws(char *str)
{
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    return str;
}

static void trim_right(char *str)
{
    int length = (int)strlen(str);
    while (length > 0 && isspace((unsigned char)str[length - 1])) {
        str[length - 1] = 0;
        length--;
    }
}

static void copy_string(char *dst, int dst_size, const char *src)
{
    if (dst_size <= 0) {
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

/* ── CURL download helpers (compiled only when libcurl is available) ──────── */

#ifdef HAVE_CURL
typedef struct {
    FILE *fp;
    sha256_ctx sha256;
} curl_write_ctx;

static size_t curl_write_and_hash_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_write_ctx *ctx = (curl_write_ctx *)userdata;
    size_t written = fwrite(ptr, size, nmemb, ctx->fp);
    sha256_update(&ctx->sha256, ptr, written);
    return written;
}

static size_t curl_write_file_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *fp = (FILE *)userdata;
    return fwrite(ptr, size, nmemb, fp);
}

static int curl_ensure_init(void)
{
    static int initialized = 0;
    if (!initialized) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            return 0;
        }
        initialized = 1;
    }
    return 1;
}

/**
 * Download url to filename.  If digest_out is non-NULL the SHA-256 of the
 * downloaded content is written into it; pass NULL to skip hash tracking.
 * Returns 1 on success, 0 on failure (partial file is deleted on failure).
 */
static int download_to_file(const char *url, const char *filename,
                            uint8_t digest_out[SHA256_DIGEST_SIZE],
                            long timeout_ms)
{
    if (!curl_ensure_init()) {
        return 0;
    }
    FILE *fp = file_open(filename, "wb");
    if (!fp) {
        return 0;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        file_close(fp);
        platform_file_manager_remove_file(filename);
        return 0;
    }

    curl_write_ctx hctx;
    hctx.fp = fp;
    sha256_init(&hctx.sha256);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, CURL_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    if (digest_out) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_and_hash_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &hctx);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    }

    CURLcode code = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    file_close(fp);

    if (code != CURLE_OK || http_code != 200) {
        platform_file_manager_remove_file(filename);
        return 0;
    }
    if (digest_out) {
        sha256_final(&hctx.sha256, digest_out);
    }
    return 1;
}
#endif /* HAVE_CURL */

/* ── Remote index handling ────────────────────────────────────────────────── */

/**
 * Download julius.list from the remote server into JULIUS_LIST_CACHE.
 * No-op (and returns 0) when CURL is unavailable.
 */
static int fetch_remote_index(void)
{
#ifdef HAVE_CURL
    if (!download_to_file(JULIUS_LIST_URL, JULIUS_LIST_CACHE, NULL, CURL_LIST_TIMEOUT_MS)) {
        log_error("CCK: failed to download julius.list", JULIUS_LIST_URL, 0);
        return 0;
    }
    return 1;
#else
    log_info("CCK: remote fetch requested but CURL support is unavailable", 0, 0);
    return 0;
#endif
}

/**
 * Parse the cached julius.list file.
 * Format per line: "<filename.map> <sha256hex>"
 * Populates data.scenarios[] with entries from the remote list.
 * Each entry has is_local set depending on whether the .map file exists.
 * Returns 1 if at least one entry was loaded, 0 otherwise.
 */
static int load_julius_list(void)
{
    FILE *fp = file_open(JULIUS_LIST_CACHE, "rb");
    if (!fp) {
        return 0;
    }

    data.num_scenarios = 0;
    char line[FILE_NAME_MAX + SHA256_HEX_SIZE + 8];
    while (fgets(line, sizeof(line), fp)) {
        char *start = skip_ws(line);
        trim_right(start);
        if (!*start || *start == '#') {
            continue;
        }

        /* Split on first whitespace: filename hash */
        char *filename = start;
        char *hash = start;
        while (*hash && !isspace((unsigned char)*hash)) {
            hash++;
        }
        if (!*hash) {
            continue;  /* line has no hash field */
        }
        *hash++ = '\0';
        hash = skip_ws(hash);
        trim_right(hash);

        if (!*filename || !*hash) {
            continue;
        }
        if (strlen(filename) >= FILE_NAME_MAX) {
            continue;
        }
        if (strlen(hash) != 64) {
            continue;  /* SHA-256 hex must be exactly 64 chars */
        }
        /* Validate hex characters */
        {
            int valid = 1;
            for (int ci = 0; hash[ci]; ci++) {
                char c = hash[ci];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    valid = 0;
                    break;
                }
            }
            if (!valid) {
                continue;
            }
        }

        if (data.num_scenarios >= MAX_CCK_MAPS) {
            break;
        }

        cck_map_entry *entry = &data.scenarios[data.num_scenarios];
        copy_string(entry->filename, FILE_NAME_MAX, filename);
        copy_string(entry->name, FILE_NAME_MAX, filename);
        file_remove_extension((uint8_t *)entry->name);
        copy_string(entry->hash, SHA256_HEX_SIZE, hash);
        entry->is_local = file_exists(filename, NOT_LOCALIZED);

        data.num_scenarios++;
    }

    file_close(fp);
    return data.num_scenarios > 0;
}

/* ── Local scenario helpers ───────────────────────────────────────────────── */

static int has_scenario_filename(const char *filename)
{
    for (int i = 0; i < data.num_scenarios; i++) {
        if (platform_file_manager_compare_filename(data.scenarios[i].filename, filename) == 0) {
            return 1;
        }
    }
    return 0;
}

static void set_local_entry(int index, const char *filename)
{
    cck_map_entry *entry = &data.scenarios[index];
    copy_string(entry->filename, FILE_NAME_MAX, filename);
    copy_string(entry->name, FILE_NAME_MAX, filename);
    file_remove_extension((uint8_t *)entry->name);
    entry->hash[0] = 0;
    entry->is_local = 1;
}

/**
 * Append any locally present .map files not already in data.scenarios[].
 * Used by both custom mode (full list) and network mode (extras not on server).
 */
static void append_local_scenarios(void)
{
    data.fallback_scenarios = dir_find_files_with_extension(MAP_FILE_EXTENSION);
    int num_files = data.fallback_scenarios->num_files;
    if (num_files > MAX_CCK_MAPS) {
        num_files = MAX_CCK_MAPS;
    }
    for (int i = 0; i < num_files; i++) {
        const char *filename = data.fallback_scenarios->files[i];
        if (has_scenario_filename(filename)) {
            continue;
        }
        if (data.num_scenarios >= MAX_CCK_MAPS) {
            break;
        }
        set_local_entry(data.num_scenarios, filename);
        data.num_scenarios++;
    }
}

static void init(void)
{
    scenario_set_custom(2);
    data.num_scenarios = 0;

    if (data.mode == CCK_SELECTION_NETWORK) {
        /* Fetch remote index; failures fall through gracefully */
        fetch_remote_index();
        load_julius_list();
    }

    /* Append any locally present maps not already listed */
    append_local_scenarios();

    data.focus_button_id = 0;
    data.focus_toggle_button = 0;
    data.focus_download_button = 0;
    data.show_minimap = 0;
    data.selected_scenario_filename[0] = 0;
    data.selected_scenario_display[0] = 0;
    button_select_item(0, 0);
    scrollbar_init(&scrollbar, 0, data.num_scenarios);
}

static void draw_scenario_list(void)
{
    inner_panel_draw(16, 210, 16, 16);
    for (int i = 0; i < MAX_SCENARIOS; i++) {
        int list_index = i + scrollbar.scroll_position;
        if (list_index >= data.num_scenarios) {
            break;
        }
        font_t font;
        if (data.focus_button_id == i + 1 ||
            (!data.focus_button_id && data.selected_item == list_index)) {
            font = FONT_NORMAL_WHITE;
        } else if (data.mode == CCK_SELECTION_NETWORK &&
                   data.scenarios[list_index].is_local) {
            /* Already downloaded: show brighter so it stands out */
            font = FONT_NORMAL_WHITE;
        } else {
            font = FONT_NORMAL_GREEN;
        }
        uint8_t displayable_file[FILE_NAME_MAX];
        encoding_from_utf8(data.scenarios[list_index].name, displayable_file, FILE_NAME_MAX);
        text_ellipsize(displayable_file, font, 240);
        text_draw(displayable_file, 24, 220 + 16 * i, font, 0);
    }
}

static void draw_metadata_line(const char *label, const char *value, int x, int y, int width)
{
    char line[METADATA_LINE_MAX];
    snprintf(line, sizeof(line), "%s: %s", label, value);
    uint8_t displayable_line[METADATA_LINE_MAX];
    encoding_from_utf8(line, displayable_line, sizeof(displayable_line));
    text_ellipsize(displayable_line, FONT_NORMAL_WHITE, width);
    text_draw(displayable_line, x, y, FONT_NORMAL_WHITE, 0);
}

static void draw_scenario_info(void)
{
    const int scenario_info_x = 335;
    const int scenario_info_width = 280;
    const int scenario_criteria_x = 420;

    image_draw(image_group(GROUP_SCENARIO_IMAGE) + scenario_image_id(), 78, 36);

    text_ellipsize(data.selected_scenario_display, FONT_LARGE_BLACK, scenario_info_width + 10);
    text_draw_centered(data.selected_scenario_display,
        scenario_info_x, 25, scenario_info_width + 10, FONT_LARGE_BLACK, 0);

    /* In network mode: show abbreviated SHA-256 from the remote index (if any) */
    if (data.mode == CCK_SELECTION_NETWORK &&
        data.selected_item >= 0 && data.selected_item < data.num_scenarios) {
        const cck_map_entry *entry = &data.scenarios[data.selected_item];
        if (entry->hash[0]) {
            /* Show first 20 hex chars + "…" to fit in the panel */
            char hash_short[24];
            snprintf(hash_short, sizeof(hash_short), "%.20s...", entry->hash);
            draw_metadata_line((const char *)translation_for(TR_CCK_METADATA_HASH_LABEL),
                hash_short, scenario_info_x + 10, 60, scenario_info_width);
        }
    }

    lang_text_draw_year(scenario_property_start_year(), scenario_criteria_x, 90, FONT_LARGE_BLACK);

    if (data.show_minimap) {
        widget_scenario_minimap_draw(332, 119, 286, 300);
        // minimap button: draw mission instructions image
        image_draw(image_group(GROUP_SIDEBAR_BRIEFING_ROTATE_BUTTONS),
            toggle_minimap_button.x + 3, toggle_minimap_button.y + 3);
    } else {
        // minimap button: draw minimap
        widget_scenario_minimap_draw(
            toggle_minimap_button.x + 3, toggle_minimap_button.y + 3,
            toggle_minimap_button.width - 6, toggle_minimap_button.height - 6
        );

        lang_text_draw_centered(44, 77 + scenario_property_climate(),
            scenario_info_x, 150, scenario_info_width, FONT_NORMAL_BLACK);

        // map size
        int text_id;
        switch (scenario_map_size()) {
            case 40: text_id = 121; break;
            case 60: text_id = 122; break;
            case 80: text_id = 123; break;
            case 100: text_id = 124; break;
            case 120: text_id = 125; break;
            default: text_id = 126; break;
        }
        lang_text_draw_centered(44, text_id, scenario_info_x, 170, scenario_info_width, FONT_NORMAL_BLACK);

        // military
        int num_invasions = scenario_invasion_count();
        if (num_invasions <= 0) {
            text_id = 112;
        } else if (num_invasions <= 2) {
            text_id = 113;
        } else if (num_invasions <= 4) {
            text_id = 114;
        } else if (num_invasions <= 10) {
            text_id = 115;
        } else {
            text_id = 116;
        }
        lang_text_draw_centered(44, text_id, scenario_info_x, 190, scenario_info_width, FONT_NORMAL_BLACK);

        lang_text_draw_centered(32, 11 + scenario_property_player_rank(),
            scenario_info_x, 210, scenario_info_width, FONT_NORMAL_BLACK);
        if (scenario_is_open_play()) {
            if (scenario_open_play_id() < 12) {
                lang_text_draw_multiline(145, scenario_open_play_id(),
                    scenario_info_x + 10, 270, scenario_info_width - 10, FONT_NORMAL_BLACK);
            }
        } else {
            lang_text_draw_centered(44, 127, scenario_info_x, 262, scenario_info_width, FONT_NORMAL_BLACK);
            int width;
            if (scenario_criteria_culture_enabled()) {
                width = text_draw_number(scenario_criteria_culture(), '@', " ",
                    scenario_criteria_x, 290, FONT_NORMAL_BLACK);
                lang_text_draw(44, 129, scenario_criteria_x + width, 290, FONT_NORMAL_BLACK);
            }
            if (scenario_criteria_prosperity_enabled()) {
                width = text_draw_number(scenario_criteria_prosperity(), '@', " ",
                    scenario_criteria_x, 306, FONT_NORMAL_BLACK);
                lang_text_draw(44, 130, scenario_criteria_x + width, 306, FONT_NORMAL_BLACK);
            }
            if (scenario_criteria_peace_enabled()) {
                width = text_draw_number(scenario_criteria_peace(), '@', " ",
                    scenario_criteria_x, 322, FONT_NORMAL_BLACK);
                lang_text_draw(44, 131, scenario_criteria_x + width, 322, FONT_NORMAL_BLACK);
            }
            if (scenario_criteria_favor_enabled()) {
                width = text_draw_number(scenario_criteria_favor(), '@', " ",
                    scenario_criteria_x, 338, FONT_NORMAL_BLACK);
                lang_text_draw(44, 132, scenario_criteria_x + width, 338, FONT_NORMAL_BLACK);
            }
            if (scenario_criteria_population_enabled()) {
                width = text_draw_number(scenario_criteria_population(), '@', " ",
                    scenario_criteria_x, 354, FONT_NORMAL_BLACK);
                lang_text_draw(44, 133, scenario_criteria_x + width, 354, FONT_NORMAL_BLACK);
            }
            if (scenario_criteria_time_limit_enabled()) {
                width = text_draw_number(scenario_criteria_time_limit_years(), '@', " ",
                    scenario_criteria_x, 370, FONT_NORMAL_BLACK);
                lang_text_draw(44, 134, scenario_criteria_x + width, 370, FONT_NORMAL_BLACK);
            }
            if (scenario_criteria_survival_enabled()) {
                width = text_draw_number(scenario_criteria_survival_years(), '@', " ",
                    scenario_criteria_x, 386, FONT_NORMAL_BLACK);
                lang_text_draw(44, 135, scenario_criteria_x + width, 386, FONT_NORMAL_BLACK);
            }
        }
    }
    lang_text_draw_centered(44, 136, scenario_info_x, 446, scenario_info_width, FONT_NORMAL_BLACK);
}

static int should_show_download_button(void)
{
    if (data.mode != CCK_SELECTION_NETWORK) {
        return 0;
    }
    if (data.selected_item < 0 || data.selected_item >= data.num_scenarios) {
        return 0;
    }
    const cck_map_entry *entry = &data.scenarios[data.selected_item];
    /* Only show when the entry has a remote hash and is not yet downloaded */
    return entry->hash[0] && !entry->is_local;
}

static void draw_background(void)
{
    image_draw_fullscreen_background(image_group(GROUP_CCK_BACKGROUND));
    graphics_in_dialog();
    inner_panel_draw(280, 242, 2, 12);
    draw_scenario_list();
    draw_scenario_info();
    graphics_reset_dialog();
}

static void draw_foreground(void)
{
    graphics_in_dialog();
    image_buttons_draw(0, 0, &start_button, 1);
    button_border_draw(
        toggle_minimap_button.x, toggle_minimap_button.y,
        toggle_minimap_button.width, toggle_minimap_button.height,
        data.focus_toggle_button);
    scrollbar_draw(&scrollbar);
    draw_scenario_list();

    if (should_show_download_button()) {
        large_label_draw(download_button.x, download_button.y,
            download_button.width / BLOCK_SIZE, data.focus_download_button ? 1 : 0);
        text_draw_centered(translation_for(TR_CCK_NETWORK_DOWNLOAD),
            download_button.x, download_button.y + 6,
            download_button.width, FONT_NORMAL_GREEN, 0);
    }

    graphics_reset_dialog();
}

static void handle_input(const mouse *m, const hotkeys *h)
{
    const mouse *m_dialog = mouse_in_dialog(m);
    if (scrollbar_handle_mouse(&scrollbar, m_dialog)) {
        return;
    }
    if (image_buttons_handle_mouse(m_dialog, 0, 0, &start_button, 1, 0)) {
        return;
    }
    if (generic_buttons_handle_mouse(m_dialog, 0, 0, &toggle_minimap_button, 1, &data.focus_toggle_button)) {
        return;
    }
    if (should_show_download_button()) {
        if (generic_buttons_handle_mouse(m_dialog, 0, 0, &download_button, 1, &data.focus_download_button)) {
            return;
        }
    } else {
        data.focus_download_button = 0;
    }
    if (generic_buttons_handle_mouse(m_dialog, 0, 0, file_buttons, MAX_SCENARIOS, &data.focus_button_id)) {
        return;
    }
    if (h->enter_pressed) {
        button_start_scenario(0, 0);
        return;
    }
    if (input_go_back_requested(m, h)) {
        window_go_back();
    }
}

static void button_select_item(int index, int param2)
{
    int selected = scrollbar.scroll_position + index;
    if (selected < 0 || selected >= data.num_scenarios) {
        return;
    }
    data.selected_item = selected;
    /* Only load scenario data for locally available files */
    if (data.scenarios[selected].is_local) {
        strcpy(data.selected_scenario_filename, data.scenarios[selected].filename);
        game_file_load_scenario_data(data.selected_scenario_filename);
    } else {
        data.selected_scenario_filename[0] = 0;
    }
    encoding_from_utf8(data.scenarios[selected].name, data.selected_scenario_display, FILE_NAME_MAX);
    window_invalidate();
}

static void button_start_scenario(int param1, int param2)
{
    if (!data.selected_scenario_filename[0]) {
        return;
    }
    if (game_file_start_scenario(data.selected_scenario_filename)) {
        sound_music_update(1);
        window_city_show();
    }
}

static void button_download_map(int param1, int param2)
{
    if (data.selected_item < 0 || data.selected_item >= data.num_scenarios) {
        return;
    }
    cck_map_entry *entry = &data.scenarios[data.selected_item];
    if (!entry->hash[0] || entry->is_local) {
        return;
    }

#ifdef HAVE_CURL
    char url[sizeof(JULIUS_MAP_BASE_URL) + FILE_NAME_MAX];
    snprintf(url, sizeof(url), "%s%s", JULIUS_MAP_BASE_URL, entry->filename);

    /* Download the map file and compute its SHA-256 simultaneously */
    uint8_t digest[SHA256_DIGEST_SIZE];
    if (!download_to_file(url, entry->filename, digest, CURL_MAP_TIMEOUT_MS)) {
        window_plain_message_dialog_show(TR_CCK_NETWORK_ERROR_TITLE, TR_CCK_NETWORK_FETCH_ERROR);
        return;
    }

    /* Verify integrity */
    char computed_hex[SHA256_HEX_SIZE];
    sha256_hex(digest, computed_hex);
    if (strcmp(computed_hex, entry->hash) != 0) {
        log_error("CCK: SHA-256 mismatch for", entry->filename, 0);
        platform_file_manager_remove_file(entry->filename);
        window_plain_message_dialog_show(TR_CCK_NETWORK_ERROR_TITLE, TR_CCK_NETWORK_HASH_ERROR);
        return;
    }

    /* Success: mark as local and load the scenario data */
    entry->is_local = 1;
    strcpy(data.selected_scenario_filename, entry->filename);
    game_file_load_scenario_data(data.selected_scenario_filename);
    window_invalidate();
#else
    window_plain_message_dialog_show(TR_CCK_NETWORK_ERROR_TITLE, TR_CCK_NETWORK_FETCH_ERROR);
#endif
}

static void button_toggle_minimap(int param1, int param2)
{
    data.show_minimap = !data.show_minimap;
    window_invalidate();
}

static void on_scroll(void)
{
    window_invalidate();
}

void window_cck_selection_show(void)
{
    data.mode = CCK_SELECTION_CUSTOM;
    window_type window = {
        WINDOW_CCK_SELECTION,
        draw_background,
        draw_foreground,
        handle_input
    };
    init();
    window_show(&window);
}

void window_cck_selection_show_network(void)
{
    data.mode = CCK_SELECTION_NETWORK;
    window_type window = {
        WINDOW_CCK_SELECTION,
        draw_background,
        draw_foreground,
        handle_input
    };
    init();
    window_show(&window);
}
