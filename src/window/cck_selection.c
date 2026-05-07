#include "cck_selection.h"

#include "core/dir.h"
#include "core/encoding.h"
#include "core/file.h"
#include "core/image_group.h"
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
#include "widget/scenario_minimap.h"
#include "window/city.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SCENARIOS 15
#define MAX_CCK_MAPS 200
#define MAP_TIME_MAX 64
#define MAP_HASH_MAX 128
#define LIST_VERSION_LINE_MAX 32
#define MAP_LIST_LINE_MAX (4 * FILE_NAME_MAX + 8)
#define METADATA_LINE_MAX (2 * FILE_NAME_MAX + 8)
#define MAP_FILE_EXTENSION "map"

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

static int read_list_version(void)
{
    FILE *fp = file_open("list.version", "rb");
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
    long version = strtol(start, &end, 10);
    if (end == start || version < 0 || version > INT_MAX) {
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
    strncpy(entry->filename, filename, FILE_NAME_MAX);
    entry->filename[FILE_NAME_MAX - 1] = 0;
    strncpy(entry->id, filename, FILE_NAME_MAX);
    entry->id[FILE_NAME_MAX - 1] = 0;
    strncpy(entry->name, filename, FILE_NAME_MAX);
    entry->name[FILE_NAME_MAX - 1] = 0;
    file_remove_extension((uint8_t*)entry->name);
    entry->time[0] = 0;
    entry->hash[0] = 0;
}

static int load_metadata_list(void)
{
    FILE *fp = file_open("maps.list", "rb");
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

        if (data.num_scenarios >= MAX_CCK_MAPS) {
            break;
        }

        cck_map_entry *entry = &data.scenarios[data.num_scenarios];
        strncpy(entry->id, id, FILE_NAME_MAX);
        entry->id[FILE_NAME_MAX - 1] = 0;
        strncpy(entry->name, name, FILE_NAME_MAX);
        entry->name[FILE_NAME_MAX - 1] = 0;
        strncpy(entry->time, time, MAP_TIME_MAX);
        entry->time[MAP_TIME_MAX - 1] = 0;
        strncpy(entry->hash, hash, MAP_HASH_MAX);
        entry->hash[MAP_HASH_MAX - 1] = 0;

        strncpy(entry->filename, id, FILE_NAME_MAX);
        entry->filename[FILE_NAME_MAX - 1] = 0;
        if (!file_has_extension(entry->filename, MAP_FILE_EXTENSION)) {
            file_append_extension(entry->filename, MAP_FILE_EXTENSION);
        }

        data.num_scenarios++;
    }

    file_close(fp);
    return data.num_scenarios > 0;
}

static void init(void)
{
    scenario_set_custom(2);
    data.list_version = read_list_version();
    data.has_metadata_list = load_metadata_list();
    if (!data.has_metadata_list) {
        data.fallback_scenarios = dir_find_files_with_extension(MAP_FILE_EXTENSION);
        data.num_scenarios = data.fallback_scenarios->num_files;
        if (data.num_scenarios > MAX_CCK_MAPS) {
            data.num_scenarios = MAX_CCK_MAPS;
        }
        for (int i = 0; i < data.num_scenarios; i++) {
            set_map_entry_from_filename(i, data.fallback_scenarios->files[i]);
        }
    }
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
            continue;
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
        draw_metadata_line("ID", entry->id, scenario_info_x + 10, 60, scenario_info_width);
        draw_metadata_line("TIME", entry->time, scenario_info_x + 10, 76, scenario_info_width);
        draw_metadata_line("HASH", entry->hash, scenario_info_x + 10, 92, scenario_info_width);
    } else if (data.list_version > 0) {
        char version_text[32];
        snprintf(version_text, sizeof(version_text), "%d", data.list_version);
        draw_metadata_line("LIST VERSION", version_text, scenario_info_x + 10, 60, scenario_info_width);
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
    window_type window = {
        WINDOW_CCK_SELECTION,
        draw_background,
        draw_foreground,
        handle_input
    };
    init();
    window_show(&window);
}
