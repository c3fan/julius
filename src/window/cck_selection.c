#include "cck_selection.h"

#include "core/dir.h"
#include "core/encoding.h"
#include "core/file.h"
#include "core/image_group.h"
#include "core/log.h"
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
#include "scenario/criteria.h"
#include "scenario/invasion.h"
#include "scenario/map.h"
#include "scenario/property.h"
#include "sound/music.h"
#include "platform/file_manager.h"
#include "translation/translation.h"
#include "widget/scenario_minimap.h"
#include "window/city.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#define MAX_SCENARIOS 15
// Keep this bounded to avoid excessive memory usage in static window state.
#define MAX_CCK_MAPS 200
#define MAP_TIME_MAX 64
#define MAP_HASH_MAX 128
#define LIST_VERSION_LINE_MAX 32
#define MAP_LIST_LINE_MAX (4 * FILE_NAME_MAX + 8)
#define METADATA_LINE_MAX (2 * FILE_NAME_MAX + 8)
#define VERSION_TEXT_MAX 32
#define MAP_FILE_EXTENSION "map"
#define LIST_VERSION_FILE "list.version"
#define MAP_LIST_FILE "maps.list"
#define CCK_URL_FILE "cck.url"
#define CCK_URL_ENV "JULIUS_CCK_BASE_URL"
#ifdef HAVE_CURL
#define CURL_CONNECT_TIMEOUT_MS 2000L
#define CURL_DOWNLOAD_TIMEOUT_MS 4000L
#endif

typedef enum {
    CCK_SELECTION_CUSTOM = 0,
    CCK_SELECTION_NETWORK = 1,
} cck_selection_mode;

typedef struct {
    char filename[FILE_NAME_MAX];
    char id[FILE_NAME_MAX];
    char name[FILE_NAME_MAX];
    char time[MAP_TIME_MAX];
    char hash[MAP_HASH_MAX];
} cck_map_entry;

static void button_select_item(int index, int param2);
static void button_start_scenario(int param1, int param2);
static void button_toggle_minimap(int param1, int param2);
static void on_scroll(void);

static image_button start_button =
    {600, 440, 27, 27, IB_NORMAL, GROUP_SIDEBAR_BUTTONS, 56, button_start_scenario, button_none, 1, 0, 1};

static generic_button toggle_minimap_button =
    {570, 87, 39, 28, button_toggle_minimap, button_none, 0, 0};

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
    int selected_item;
    int show_minimap;
    int list_version;
    int has_metadata_list;
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

static int read_line_from_file(const char *filename, char *buffer, int buffer_size)
{
    FILE *fp = file_open(filename, "rb");
    if (!fp) {
        return 0;
    }
    if (!fgets(buffer, buffer_size, fp)) {
        file_close(fp);
        return 0;
    }
    file_close(fp);
    char *start = skip_ws(buffer);
    trim_right(start);
    if (start != buffer) {
        memmove(buffer, start, strlen(start) + 1);
    }
    return buffer[0] != 0;
}

static int get_remote_base_url(char *buffer, int buffer_size)
{
    if (read_line_from_file(CCK_URL_FILE, buffer, buffer_size)) {
        return 1;
    }
    const char *env_url = getenv(CCK_URL_ENV);
    if (!env_url || !*env_url) {
        return 0;
    }
    copy_string(buffer, buffer_size, env_url);
    trim_right(buffer);
    return buffer[0] != 0;
}

static int is_valid_remote_base_url(const char *url)
{
    if (!url || !*url) {
        return 0;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return 0;
    }
    for (const char *p = url; *p; p++) {
        if ((unsigned char)*p < 32) {
            return 0;
        }
    }
    return 1;
}

#ifdef HAVE_CURL
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *fp = (FILE*)userdata;
    return fwrite(ptr, size, nmemb, fp);
}

static int download_to_file(const char *url, const char *filename)
{
    static int curl_initialized = 0;
    if (!curl_initialized) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            return 0;
        }
        curl_initialized = 1;
    }
    FILE *fp = file_open(filename, "wb");
    if (!fp) {
        return 0;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        file_close(fp);
        return 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, CURL_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, CURL_DOWNLOAD_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    CURLcode code = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    file_close(fp);
    if (code != CURLE_OK || http_code != 200) {
        platform_file_manager_remove_file(filename);
        return 0;
    }
    return 1;
}
#endif

static void refresh_remote_lists(void)
{
    if (data.mode != CCK_SELECTION_NETWORK) {
        return;
    }

    char base_url[FILE_NAME_MAX];
    if (!get_remote_base_url(base_url, sizeof(base_url))) {
        return;
    }
    if (!is_valid_remote_base_url(base_url)) {
        log_error("CCK: invalid remote base URL", base_url, 0);
        return;
    }

#ifdef HAVE_CURL
    char version_url[2 * FILE_NAME_MAX];
    char list_url[2 * FILE_NAME_MAX];
    snprintf(version_url, sizeof(version_url), "%s/%s", base_url, LIST_VERSION_FILE);
    snprintf(list_url, sizeof(list_url), "%s/%s", base_url, MAP_LIST_FILE);
    if (!download_to_file(version_url, LIST_VERSION_FILE)) {
        log_error("CCK: failed to download list.version", version_url, 0);
    }
    if (!download_to_file(list_url, MAP_LIST_FILE)) {
        log_error("CCK: failed to download maps.list", list_url, 0);
    }
#else
    log_info("CCK: remote refresh requested but curl support is unavailable", 0, 0);
#endif
}

static int read_list_version(void)
{
    FILE *fp = file_open(LIST_VERSION_FILE, "rb");
    if (!fp) {
        return 0;
    }

    char line[LIST_VERSION_LINE_MAX];
    if (!fgets(line, sizeof(line), fp)) {
        file_close(fp);
        return 0;
    }
    file_close(fp);

    char *start = skip_ws(line);
    if (!*start) {
        return 0;
    }

    char *end = 0;
    errno = 0;
    long version = strtol(start, &end, 10);
    if (end == start || errno == ERANGE || version < 0 || version > INT_MAX) {
        return 0;
    }
    end = skip_ws(end);
    if (*end) {
        return 0;
    }
    return (int)version;
}

static void set_map_entry_from_filename(int index, const char *filename)
{
    cck_map_entry *entry = &data.scenarios[index];
    copy_string(entry->filename, FILE_NAME_MAX, filename);
    copy_string(entry->id, FILE_NAME_MAX, filename);
    copy_string(entry->name, FILE_NAME_MAX, filename);
    file_remove_extension((uint8_t*)entry->name);
    entry->time[0] = 0;
    entry->hash[0] = 0;
}

static int load_metadata_list(void)
{
    FILE *fp = file_open(MAP_LIST_FILE, "rb");
    if (!fp) {
        return 0;
    }

    data.num_scenarios = 0;
    char line[MAP_LIST_LINE_MAX];
    while (fgets(line, sizeof(line), fp)) {
        char *start = skip_ws(line);
        trim_right(start);
        if (!*start || *start == '#') {
            continue;
        }

        char *id = start;
        char *name = strchr(id, '|');
        if (!name) {
            continue;
        }
        *name++ = 0;

        char *time = strchr(name, '|');
        if (!time) {
            continue;
        }
        *time++ = 0;

        char *hash = strchr(time, '|');
        if (!hash) {
            continue;
        }
        *hash++ = 0;

        id = skip_ws(id);
        name = skip_ws(name);
        time = skip_ws(time);
        hash = skip_ws(hash);
        trim_right(id);
        trim_right(name);
        trim_right(time);
        trim_right(hash);
        if (!*id || !*name || !*time || !*hash) {
            continue;
        }
        if (strlen(id) >= FILE_NAME_MAX || strlen(name) >= FILE_NAME_MAX ||
            strlen(time) >= MAP_TIME_MAX || strlen(hash) >= MAP_HASH_MAX) {
            continue;
        }

        if (data.num_scenarios >= MAX_CCK_MAPS) {
            break;
        }

        cck_map_entry *entry = &data.scenarios[data.num_scenarios];
        copy_string(entry->id, FILE_NAME_MAX, id);
        copy_string(entry->name, FILE_NAME_MAX, name);
        copy_string(entry->time, MAP_TIME_MAX, time);
        copy_string(entry->hash, MAP_HASH_MAX, hash);

        copy_string(entry->filename, FILE_NAME_MAX, id);
        if (!file_has_extension(entry->filename, MAP_FILE_EXTENSION)) {
            file_append_extension(entry->filename, MAP_FILE_EXTENSION);
        }

        data.num_scenarios++;
    }

    file_close(fp);
    return data.num_scenarios > 0;
}

static int has_scenario_filename(const char *filename)
{
    for (int i = 0; i < data.num_scenarios; i++) {
        if (platform_file_manager_compare_filename(data.scenarios[i].filename, filename) == 0) {
            return 1;
        }
    }
    return 0;
}

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
        set_map_entry_from_filename(data.num_scenarios, filename);
        data.num_scenarios++;
    }
}

static void init(void)
{
    scenario_set_custom(2);
    data.num_scenarios = 0;
    data.list_version = 0;
    data.has_metadata_list = 0;

    if (data.mode == CCK_SELECTION_NETWORK) {
        refresh_remote_lists();
        data.list_version = read_list_version();
        data.has_metadata_list = load_metadata_list();
    }

    append_local_scenarios();

    data.focus_button_id = 0;
    data.focus_toggle_button = 0;
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
        font_t font = FONT_NORMAL_GREEN;
        if (data.focus_button_id == i + 1) {
            font = FONT_NORMAL_WHITE;
        } else if (!data.focus_button_id && data.selected_item == list_index) {
            font = FONT_NORMAL_WHITE;
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
    if (data.has_metadata_list && data.selected_item >= 0 && data.selected_item < data.num_scenarios) {
        const cck_map_entry *entry = &data.scenarios[data.selected_item];
        draw_metadata_line((const char*)translation_for(TR_CCK_METADATA_ID_LABEL), entry->id, scenario_info_x + 10, 60, scenario_info_width);
        draw_metadata_line((const char*)translation_for(TR_CCK_METADATA_TIME_LABEL), entry->time, scenario_info_x + 10, 76, scenario_info_width);
        draw_metadata_line((const char*)translation_for(TR_CCK_METADATA_HASH_LABEL), entry->hash, scenario_info_x + 10, 92, scenario_info_width);
    } else if (data.list_version > 0) {
        char version_text[VERSION_TEXT_MAX];
        snprintf(version_text, sizeof(version_text), "%d", data.list_version);
        draw_metadata_line((const char*)translation_for(TR_CCK_METADATA_LIST_VERSION_LABEL), version_text, scenario_info_x + 10, 60, scenario_info_width);
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
    strcpy(data.selected_scenario_filename, data.scenarios[data.selected_item].filename);
    game_file_load_scenario_data(data.selected_scenario_filename);
    encoding_from_utf8(data.scenarios[data.selected_item].name, data.selected_scenario_display, FILE_NAME_MAX);
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
