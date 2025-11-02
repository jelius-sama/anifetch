#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <wordexp.h>

#define MAX_LINES 100
#define MAX_LINE_LENGTH 512
#define MAX_PATH 1024

typedef struct {
    int img_width;
    int text_offset;
    char image_path[MAX_PATH];
    char crop_mode[16]; // "auto" or "fill"
} Config;

int get_terminal_width() {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_col;
}

char* expand_path(const char *path) {
    wordexp_t exp_result;
    char *expanded = malloc(MAX_PATH);
    
    if (wordexp(path, &exp_result, 0) == 0) {
        strncpy(expanded, exp_result.we_wordv[0], MAX_PATH - 1);
        expanded[MAX_PATH - 1] = '\0';
        wordfree(&exp_result);
    } else {
        strncpy(expanded, path, MAX_PATH - 1);
        expanded[MAX_PATH - 1] = '\0';
    }
    
    return expanded;
}

void load_config(Config *config, const char *config_path) {
    // Set defaults
    config->img_width = 40;
    config->text_offset = 5;
    config->image_path[0] = '\0';
    strcpy(config->crop_mode, "auto");

    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        fprintf(stderr, "Warning: Could not open config file '%s', using defaults\n", config_path);
        return;
    }

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), fp)) {
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        // Parse key=value
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        // Trim whitespace
        while (*key == ' ' || *key == '\t') key++;
        while (*value == ' ' || *value == '\t') value++;

        if (strcmp(key, "img_width") == 0) {
            config->img_width = atoi(value);
        } else if (strcmp(key, "text_offset") == 0) {
            config->text_offset = atoi(value);
        } else if (strcmp(key, "image_path") == 0) {
            // Expand the path with tilde support
            char *expanded = expand_path(value);
            strncpy(config->image_path, expanded, MAX_PATH - 1);
            free(expanded);
        } else if (strcmp(key, "crop_mode") == 0) {
            strncpy(config->crop_mode, value, sizeof(config->crop_mode) - 1);
        }
    }

    fclose(fp);
}

int main(int argc, char *argv[]) {
    Config config;
    
    // Expand config path
    char *config_path = expand_path("~/.config/anime/config.conf");
    
    // Load configuration
    load_config(&config, config_path);
    free(config_path);

    // Command line argument overrides config file
    const char *image_path = NULL;
    if (argc >= 2) {
        image_path = argv[1];
    } else if (config.image_path[0] != '\0') {
        image_path = config.image_path;
    } else {
        fprintf(stderr, "Usage: %s <image.jpg>\n", argv[0]);
        fprintf(stderr, "Or set 'image_path' in ~/.config/anime/config.conf\n");
        return 1;
    }
    
    // Check if file exists
    if (access(image_path, F_OK) != 0) {
        fprintf(stderr, "Error: Image file '%s' not found\n", image_path);
        return 1;
    }

    // Get neofetch output
    FILE *neofetch_pipe = popen("neofetch --off 2>/dev/null", "r");
    if (!neofetch_pipe) {
        fprintf(stderr, "Error: Could not run neofetch\n");
        return 1;
    }

    char neofetch_lines[MAX_LINES][MAX_LINE_LENGTH];
    int neofetch_line_count = 0;
    
    while (fgets(neofetch_lines[neofetch_line_count], MAX_LINE_LENGTH, neofetch_pipe) != NULL && 
           neofetch_line_count < MAX_LINES) {
        // Remove newline
        size_t len = strlen(neofetch_lines[neofetch_line_count]);
        if (len > 0 && neofetch_lines[neofetch_line_count][len-1] == '\n') {
            neofetch_lines[neofetch_line_count][len-1] = '\0';
        }
        neofetch_line_count++;
    }
    pclose(neofetch_pipe);

    // Clear the screen before rendering
    system("clear");

    // Build kitten icat command with placement
    char cmd[1024];
    
    // Determine scale mode based on crop_mode
    if (strcmp(config.crop_mode, "fill") == 0) {
        // fill mode: crop to square, centered
        snprintf(cmd, sizeof(cmd), 
                 "kitten icat --align left --place %dx%d@0x0 --scale-up '%s'",
                 config.img_width, neofetch_line_count, image_path);
    } else {
        // auto mode: preserve aspect ratio
        snprintf(cmd, sizeof(cmd), 
                 "kitten icat --align left --place %dx%d@0x0 --scale-up --background none '%s'",
                 config.img_width, neofetch_line_count, image_path);
    }
    
    // Display the image (it will be on the left)
    system(cmd);

    // Move cursor to the right of the image and print neofetch lines
    for (int i = 0; i < neofetch_line_count; i++) {
        // Move cursor to column after image
        printf("\033[%d;%dH", i + 1, config.img_width + config.text_offset);
        printf("%s", neofetch_lines[i]);
    }
    
    // Move cursor to the line right after neofetch text
    printf("\033[%d;1H", neofetch_line_count + 1);
    fflush(stdout);

    return 0;
}
