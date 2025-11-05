#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <wordexp.h>
#include <pthread.h>

/*********************************************************
    *                  Disclaimer
    *     This program is Vibe Coded with claude.ai
    * ****************************************************/

#define MAX_LINES 100
#define MAX_LINE_LENGTH 512
#define MAX_PATH 1024
#define MAX_CMD 2048

typedef struct {
    int img_width;
    int text_offset;
    char image_path[MAX_PATH];
    char crop_mode[16]; // "auto" or "fill"
} Config;

typedef struct {
    char lines[MAX_LINES][MAX_LINE_LENGTH];
    int line_count;
    int success;
} NeofetchResult;

typedef struct {
    char command[MAX_CMD];
    int success;
} ImageResult;

typedef struct {
    Config *config;
    const char *image_path;
    ImageResult *result;
} ImageThreadArgs;

typedef struct {
    NeofetchResult *result;
} NeofetchThreadArgs;

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

void* neofetch_thread(void *arg) {
    NeofetchThreadArgs *args = (NeofetchThreadArgs*)arg;
    NeofetchResult *result = args->result;
    
    result->line_count = 0;
    result->success = 0;

    // Get neofetch output
    FILE *neofetch_pipe = popen("neofetch --off 2>/dev/null", "r");
    if (!neofetch_pipe) {
        fprintf(stderr, "Error: Could not run neofetch\n");
        return NULL;
    }

    while (fgets(result->lines[result->line_count], MAX_LINE_LENGTH, neofetch_pipe) != NULL && 
           result->line_count < MAX_LINES) {
        // Remove newline
        size_t len = strlen(result->lines[result->line_count]);
        if (len > 0 && result->lines[result->line_count][len-1] == '\n') {
            result->lines[result->line_count][len-1] = '\0';
        }
        result->line_count++;
    }
    pclose(neofetch_pipe);
    
    result->success = 1;
    return NULL;
}

void* image_thread(void *arg) {
    ImageThreadArgs *args = (ImageThreadArgs*)arg;
    ImageResult *result = args->result;
    
    result->success = 0;

    // Build kitten icat command with placement
    int snprintf_result;
    
    // Determine scale mode based on crop_mode
    if (strcmp(args->config->crop_mode, "fill") == 0) {
        // fill mode: crop to square (width x width), centered
        snprintf_result = snprintf(result->command, sizeof(result->command), 
                 "kitten icat --align left --place %dx%d@0x0 --scale-up '%s'",
                 args->config->img_width, args->config->img_width, args->image_path);
    } else {
        // auto mode: preserve aspect ratio, height auto-scales based on width
        snprintf_result = snprintf(result->command, sizeof(result->command), 
                 "kitten icat --align left --place %dx0@0x0 --scale-up --background none '%s'",
                 args->config->img_width, args->image_path);
    }
    
    // Check for truncation
    if (snprintf_result >= (int)sizeof(result->command)) {
        fprintf(stderr, "Error: Command string too long\n");
        return NULL;
    }
    
    result->success = 1;
    return NULL;
}

int main(int argc, char *argv[]) {
    // Handle flags
    if (argc >= 2) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Usage: %s [OPTIONS] [IMAGE_PATH]\n\n", argv[0]);
            printf("Display system information with an anime image\n\n");
            printf("Options:\n");
            printf("  -h, --help       Show this help message\n");
            printf("  -v, --version    Show version information\n");
            printf("  IMAGE_PATH       Path to image (overrides config)\n\n");
            printf("Config file: ~/.config/anifetch/config.conf\n");
            return 0;
        }
        
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
            printf("anifetch v1.0.0\n");
            printf("A side-by-side neofetch with kitty graphics protocol\n");
            return 0;
        }
    }

    Config config;
    
    // Expand config path
    char *config_path = expand_path("~/.config/anifetch/config.conf");
    
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
        fprintf(stderr, "Or set 'image_path' in ~/.config/anifetch/config.conf\n");
        return 1;
    }
    
    // Check if file exists
    if (access(image_path, F_OK) != 0) {
        fprintf(stderr, "Error: Image file '%s' not found\n", image_path);
        return 1;
    }

    // Prepare thread data structures
    NeofetchResult neofetch_result;
    ImageResult image_result;
    
    NeofetchThreadArgs neofetch_args = {
        .result = &neofetch_result
    };
    
    ImageThreadArgs image_args = {
        .config = &config,
        .image_path = image_path,
        .result = &image_result
    };
    
    // Create both threads in parallel
    pthread_t neofetch_tid, image_tid;
    
    if (pthread_create(&neofetch_tid, NULL, neofetch_thread, &neofetch_args) != 0) {
        fprintf(stderr, "Error: Failed to create neofetch thread\n");
        return 1;
    }
    
    if (pthread_create(&image_tid, NULL, image_thread, &image_args) != 0) {
        fprintf(stderr, "Error: Failed to create image thread\n");
        pthread_join(neofetch_tid, NULL); // Clean up first thread
        return 1;
    }
    
    // Wait for both threads to complete
    if (pthread_join(neofetch_tid, NULL) != 0) {
        fprintf(stderr, "Error: Failed to join neofetch thread\n");
        return 1;
    }
    
    if (pthread_join(image_tid, NULL) != 0) {
        fprintf(stderr, "Error: Failed to join image thread\n");
        return 1;
    }
    
    // Check both threads succeeded
    if (!neofetch_result.success) {
        fprintf(stderr, "Error: Neofetch thread failed\n");
        return 1;
    }
    
    if (!image_result.success) {
        fprintf(stderr, "Error: Image thread failed\n");
        return 1;
    }

    // Both threads completed successfully, now render in main thread
    // Clear the screen before rendering
    if (system("clear") != 0) {
        fprintf(stderr, "Warning: Failed to clear screen\n");
    }

    // Display the image (it will be on the left)
    if (system(image_result.command) != 0) {
        fprintf(stderr, "Warning: Failed to display image\n");
    }

    // Move cursor to the right of the image and print neofetch lines
    for (int i = 0; i < neofetch_result.line_count; i++) {
        // Move cursor to column after image
        printf("\033[%d;%dH", i + 1, config.img_width + config.text_offset);
        printf("%s", neofetch_result.lines[i]);
    }
    
    // Move cursor to the line right after neofetch text
    printf("\033[%d;1H", neofetch_result.line_count + 1);
    fflush(stdout);

    return 0;
}
