// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jelius Basumatary <personal@jelius.dev>

/*
 * anifetch - side-by-side anime image + system fetch
 *
 * Inner workings (redesigned with some backwards compatibility):
 *
 *   anifetch
 *     -> no arg: read image_path from config, strip filename, pick random
 *        image_*.ext file from that directory.
 *
 *   anifetch /some/dir
 *     -> arg is directory: open dir, grab first file to anchor the path into
 *        config.image_path, then strip filename and re-scan dir for image_
 *        files, pick one randomly. If the picked file ends in .gpg it is
 *        treated as an encrypted image: ask for the passphrase, decrypt it
 *        in memory with gpg and feed the bytes to kitten icat. No password
 *        is stored anywhere.
 *
 *   anifetch /some/file.png
 *     -> arg is a plain file: use it directly, no password, no scanning.
 *
 * Patch accumulated over commits & versions (this tool had a messy history 😉):
 * Original : display image from config path directly.
 * Patch 1  : treat config path as a directory hint; pick random image_ file.
 * Patch 2  : CLI dir arg -> first-entry path anchor -> same patch-1 scan
 *            re-used.
 * Patch 3  : images can be pre-encrypted (.gpg); a runtime passphrase
 *            decrypts them in memory.
 *
 * NOTE: Config file is backwards compatible in the sense that it doesn't need
 * to be updated at all though depending on version it may work differently. For
 * example, if you are using the first commit of this tool then image path is
 * directly taken from config file, however, if you are using the tool after
 * patch 1 was applied then the path from the config turns into a hint where
 * images are stored, we strip the filename after the last slash in pathname and
 * then search for images in that directory according to our searching protocol.
 *
 * INFO: Encryption is currently detected by the ".gpg" extension of the
 * picked file. The intended design is a secret directory of pre-encrypted
 * images that only someone who knows the passphrase can ever view (no
 * password is stored, gpg proves the passphrase by decrypting).
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Wipe that the compiler can't optimise away (memset before free can be
 * elided). */
static void secure_wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--)
        *v++ = 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* Read passphrase with echo off. Returns 1 on success. */
static int read_passphrase(char *buf, size_t sz) {
    struct termios old, silent;
    int is_tty = isatty(STDIN_FILENO);
    if (is_tty) {
        tcgetattr(STDIN_FILENO, &old);
        silent = old;
        silent.c_lflag &= ~(unsigned)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &silent);
    }
    fprintf(stderr, "Passphrase: ");
    fflush(stderr);
    int ok = fgets(buf, sz, stdin) != NULL;
    if (is_tty)
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
    fprintf(stderr, "\n");
    if (ok)
        buf[strcspn(buf, "\n")] = '\0';
    return ok;
}

/*
 * decrypt_to_memory:
 *   Runs gpg on `path`, feeding the passphrase on fd 3 (never argv, so it
 *   can't show up in `ps`), and collects plaintext from gpg's stdout into a
 *   malloc'd buffer. Returns 1 on success; caller must wipe + free *out.
 */
static int decrypt_to_memory(const char *path, const char *pass,
                             unsigned char **out, size_t *out_len) {
    int outp[2], passp[2];
    if (pipe(outp) != 0)
        return 0;
    if (pipe(passp) != 0) {
        close(outp[0]);
        close(outp[1]);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 0;
    }

    if (pid == 0) {
        close(outp[0]); /* parent's read end; may be fd 3, so close BEFORE using
                           fd 3 */
        dup2(outp[1], STDOUT_FILENO);
        if (passp[0] != 3)
            dup2(passp[0], 3); /* passphrase arrives on fd 3 */
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDERR_FILENO);
        }
        for (int fd = 4; fd < 64;
             fd++) /* closes outp[1], passp[0], passp[1], dn, strays */
            close(fd);
        char *const argv[] = {"gpg",
                              "--batch",
                              "--quiet",
                              "--yes",
                              "--pinentry-mode",
                              "loopback",
                              "--passphrase-fd",
                              "3",
                              "-d",
                              (char *)path,
                              NULL};
        execvp(argv[0], argv);
        _exit(127);
    }

    close(outp[1]);
    close(passp[0]);

    /* passphrase is tiny, far below the pipe buffer, so this can't block */
    write(passp[1], pass, strlen(pass));
    write(passp[1], "\n", 1);
    close(passp[1]);

    size_t cap = 1 << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) {
        close(outp[0]);
        waitpid(pid, NULL, 0);
        return 0;
    }
    mlock(buf, cap); /* best effort: keep plaintext out of swap */

    ssize_t r;
    for (;;) {
        if (len == cap) {
            unsigned char *nb = malloc(cap * 2);
            if (!nb)
                break;
            mlock(nb, cap * 2);
            memcpy(nb, buf, len);
            secure_wipe(buf, cap);
            munlock(buf, cap);
            free(buf);
            buf = nb;
            cap *= 2;
        }
        r = read(outp[0], buf + len, cap - len);
        if (r <= 0)
            break;
        len += (size_t)r;
    }
    close(outp[0]);

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || len == 0) {
        secure_wipe(buf, cap);
        munlock(buf, cap);
        free(buf);
        return 0;
    }
    *out = buf;
    *out_len = len;
    return 1;
}

#define MAX_LINES 100
#define MAX_LINE_LEN 512
#define MAX_PATH_LEN 1024
#define MAX_FILES 256 /* max image_ files in a dir */
#define MAX_FNAME 256

#define VERSION "2.0.0"

typedef struct {
    int img_width;   /* terminal columns for the image placement */
    int img_height;  /* 0 = fallback to fetch_line_count */
    int text_offset; /* columns between image right-edge and text */
    char image_path[MAX_PATH_LEN];
    char crop_mode[16]; /* "auto" | "fill" */
    char backend[16];   /* "fastfetch" | "neofetch" */
} Config;

/*
 * expand_path: tilde / variable expansion
 * Caller must free() the returned pointer.
 */
static char *expand_path(const char *path) {
    char *out = malloc(MAX_PATH_LEN);
    if (!out) {
        perror("malloc");
        exit(1);
    }

    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home)
            home = ""; /* degenerate: no HOME set */
        snprintf(out, MAX_PATH_LEN, "%s%s", home, path + 1);
    } else {
        strncpy(out, path, MAX_PATH_LEN - 1);
        out[MAX_PATH_LEN - 1] = '\0';
    }
    return out;
}

static void load_config(Config *cfg, const char *path) {
    cfg->img_width = 40;
    cfg->img_height = 0; /* 0 = not set; use fetch_line_count */
    cfg->text_offset = 5;
    cfg->image_path[0] = '\0';
    strncpy(cfg->crop_mode, "auto", sizeof(cfg->crop_mode) - 1);
    strncpy(cfg->backend, "fastfetch", sizeof(cfg->backend) - 1);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Warning: cannot open config '%s', using defaults\n",
                path);
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* strip newline */
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = '\0';

        if (line[0] == '\0' || line[0] == '#')
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';

        char *key = line, *val = eq + 1;

        /* trim leading whitespace */
        while (*key == ' ' || *key == '\t')
            key++;
        while (*val == ' ' || *val == '\t')
            val++;

        if (strcmp(key, "img_width") == 0) {
            cfg->img_width = atoi(val);
        } else if (strcmp(key, "img_height") == 0) {
            cfg->img_height = atoi(val);
        } else if (strcmp(key, "text_offset") == 0) {
            cfg->text_offset = atoi(val);
        } else if (strcmp(key, "crop_mode") == 0) {
            strncpy(cfg->crop_mode, val, sizeof(cfg->crop_mode) - 1);
        } else if (strcmp(key, "backend") == 0) {
            strncpy(cfg->backend, val, sizeof(cfg->backend) - 1);
        } else if (strcmp(key, "image_path") == 0) {
            char *exp = expand_path(val);
            strncpy(cfg->image_path, exp, MAX_PATH_LEN - 1);
            free(exp);
        }
    }
    fclose(fp);
}

/*
 * pick_random_image_from_dir:
 *   Scans dir_path for files whose name begins with "image_".
 *   Picks one at random and writes the full path into out (size out_sz).
 *   Returns 1 on success, 0 if no matching files found.
 */
static int pick_random_image_from_dir(const char *dir_path, char *out,
                                      size_t out_sz) {
    DIR *dr = opendir(dir_path);
    if (!dr) {
        perror("opendir");
        return 0;
    }

    char names[MAX_FILES][MAX_FNAME];
    int count = 0;

    struct dirent *de;
    while ((de = readdir(dr)) != NULL && count < MAX_FILES) {
        if (strncmp(de->d_name, "image_", 6) == 0) {
            strncpy(names[count], de->d_name, MAX_FNAME - 1);
            names[count][MAX_FNAME - 1] = '\0';
            count++;
        }
    }
    closedir(dr);

    if (count == 0)
        return 0;

    srand((unsigned)time(NULL));
    int idx = rand() % count;

    snprintf(out, out_sz, "%s/%s", dir_path, names[idx]);
    return 1;
}

/*
 * dir_to_image_path:
 *   Given a directory path, grabs the first non-dot entry to anchor the
 *   path (legacy Patch-2 requirement), writes full path into cfg->image_path,
 *   then delegates to pick_random_image_from_dir for the actual random pick.
 *
 *   Returns 1 on success (image_path_out is set), 0 on failure.
 */
static int dir_to_image_path(const char *dir_path, Config *cfg,
                             char *image_path_out, size_t out_sz) {
    /* ── Step 1 (Patch 2 legacy): anchor first non-dot entry into
     *   cfg->image_path so the path pipeline has something to strip. ── */
    DIR *dir = opendir(dir_path);
    if (!dir) {
        perror("opendir");
        return 0;
    }

    struct dirent *entry;
    char anchor_file[MAX_FNAME] = {0};

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        strncpy(anchor_file, entry->d_name, MAX_FNAME - 1);
        break;
    }
    closedir(dir);

    if (anchor_file[0] == '\0') {
        fprintf(stderr, "Error: directory '%s' is empty\n", dir_path);
        return 0;
    }

    /* build the anchored path into cfg->image_path */
    snprintf(cfg->image_path, MAX_PATH_LEN, "%s/%s", dir_path, anchor_file);

    /* ── Step 2 (Patch 1 logic): strip filename, scan for image_ files ── */
    return pick_random_image_from_dir(dir_path, image_path_out, out_sz);
}

/*
 * exec_and_capture:
 *   Forks, execs argv[0] with argv, captures stdout into lines[][].
 *   Returns number of lines read, -1 on error.
 *
 *   This replaces popen() — no shell, no PATH injection risk.
 */
static int exec_and_capture(char *const argv[], char lines[][MAX_LINE_LEN],
                            int max_lines) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* child: redirect stdout → write-end of pipe */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            perror("dup2");
            _exit(1);
        }
        close(pipefd[1]);

        /* silence stderr */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execvp(argv[0], argv);
        /* execvp only returns on error */
        _exit(127);
    }

    /* parent: read from read-end */
    close(pipefd[1]);

    FILE *fp = fdopen(pipefd[0], "r");
    if (!fp) {
        perror("fdopen");
        waitpid(pid, NULL, 0);
        return -1;
    }

    int count = 0;
    while (count < max_lines && fgets(lines[count], MAX_LINE_LEN, fp) != NULL) {
        size_t n = strlen(lines[count]);
        if (n > 0 && lines[count][n - 1] == '\n')
            lines[count][n - 1] = '\0';
        count++;
    }
    fclose(fp); /* also closes pipefd[0] */

    int status;
    waitpid(pid, &status, 0);

    return count;
}

/*
 * exec_no_capture:
 *   Forks and execs argv, inheriting stdout/stderr.
 *   Waits for completion. Returns exit status, -1 on fork/exec error.
 *
 *   This replaces system() — no shell spawned.
 */
static int exec_no_capture(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_fastfetch(char lines[][MAX_LINE_LEN], int max_lines) {
    /*
     * Original used: script -qfc "fastfetch --logo none" /dev/null
     * That was needed to force fastfetch to think it has a TTY (for ANSI
     * colors). We replicate that by keeping the script wrapper since
     * fastfetch checks isatty().
     */
    char *const argv[] = {"script", "-qfc", "fastfetch --logo none",
                          "/dev/null", NULL};
    return exec_and_capture(argv, lines, max_lines);
}

static int run_neofetch(char lines[][MAX_LINE_LEN], int max_lines) {
    char *const argv[] = {"neofetch", "--off", NULL};
    return exec_and_capture(argv, lines, max_lines);
}

static void clear_screen(void) {
    /* ANSI: clear screen + move cursor to home */
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
}

/*
 * run_icat:
 *   Builds the kitten icat command exactly as it was before (fine-tuned,
 *   do not alter the argument structure) and exec's it via fork.
 *
 *   place_h: terminal rows for the --place argument (img_height or
 *            fetch_line_count as fallback).
 */
static void run_icat(const Config *cfg, const char *image_path, int place_h) {
    char place_arg[64];
    snprintf(place_arg, sizeof(place_arg), "%dx%d@0x0", cfg->img_width,
             place_h);

    if (strcmp(cfg->crop_mode, "fill") == 0) {
        char *const argv[] = {"kitten",     "icat",
                              "--stdin=no", "--transfer-mode=stream",
                              "--align",    "left",
                              "--place",    place_arg,
                              "--scale-up", (char *)image_path,
                              NULL};
        exec_no_capture(argv);
    } else {
        /* auto: preserve aspect ratio, transparent background */
        char *const argv[] = {"kitten",     "icat",
                              "--stdin=no", "--transfer-mode=stream",
                              "--align",    "left",
                              "--place",    place_arg,
                              "--scale-up", "--background",
                              "none",       (char *)image_path,
                              NULL};
        exec_no_capture(argv);
    }
}

static void run_icat_mem(const Config *cfg, const unsigned char *data,
                         size_t len, int place_h) {
    char place_arg[64];
    snprintf(place_arg, sizeof(place_arg), "%dx%d@0x0", cfg->img_width,
             place_h);

    int fd[2];
    if (pipe(fd) != 0) {
        perror("pipe");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);
        close(fd[1]);
        if (strcmp(cfg->crop_mode, "fill") == 0) {
            char *const a[] = {
                "kitten",     "icat", "--stdin=yes", "--transfer-mode=stream",
                "--align",    "left", "--place",     place_arg,
                "--scale-up", NULL};
            execvp(a[0], a);
        } else {
            char *const a[] = {"kitten",      "icat",
                               "--stdin=yes", "--transfer-mode=stream",
                               "--align",     "left",
                               "--place",     place_arg,
                               "--scale-up",  "--background",
                               "none",        NULL};
            execvp(a[0], a);
        }
        _exit(127);
    }

    close(fd[0]);
    signal(SIGPIPE, SIG_IGN); /* icat dying early shouldn't kill us */
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd[1], data + off, len - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    close(fd[1]); /* EOF tells icat the image is complete */
    waitpid(pid, NULL, 0);
}

int main(int argc, char *argv[]) {
    /* refuse root because usually root previledge is given using `sudo` which
     * strips the environment variables and causes render issues due to display
     * related env being missing */
    if (geteuid() == 0) {
        fprintf(stderr, "Run without sudo privilege!\n");
        return 1;
    }

    /* ── flags ── */
    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            printf("Usage: %s [OPTIONS] [PATH]\n\n", argv[0]);
            printf("Display system info alongside an anime image.\n\n");
            printf("Options:\n");
            printf("  -h, --help       Show this help\n");
            printf("  -v, --version    Show version\n\n");
            printf("Arguments:\n");
            printf("  (none)           Use image_path from config\n");
            printf("  FILE             Display this image directly\n");
            printf("  DIR              Pick random image_ file from DIR"
                   " (.gpg files ask for a passphrase)\n\n");
            printf("Config: ~/.config/anifetch/config.conf\n");
            printf("\nConfig keys:\n");
            printf("  img_width    = <cols>   (default: 40)\n");
            printf(
                "  img_height   = <rows>   (default: 0, use fetch height)\n");
            printf("  text_offset  = <cols>   (default: 5)\n");
            printf("  crop_mode    = auto|fill\n");
            printf("  backend      = fastfetch|neofetch\n");
            printf("  image_path   = /path/to/dir/image_01.png\n");
            return 0;
        }
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
            printf("anifetch v%s\n", VERSION);
            printf("Side-by-side fetch with kitty graphics protocol\n");
            return 0;
        }
    }

    /* ── load config ── */
    Config cfg;
    char *cfg_path = expand_path("~/.config/anifetch/config.conf");
    load_config(&cfg, cfg_path);
    free(cfg_path);

    /* ── resolve image path ── */
    char resolved_image[MAX_PATH_LEN] = {0};
    int have_image = 0;

    if (argc >= 2) {
        const char *arg = argv[1];
        struct stat st;

        if (stat(arg, &st) != 0) {
            perror("stat");
            return 1;
        }

        if (S_ISDIR(st.st_mode)) {
            have_image = dir_to_image_path(arg, &cfg, resolved_image,
                                           sizeof(resolved_image));
            if (!have_image) {
                fprintf(stderr, "Error: no image_ files found in '%s'\n", arg);
                return 1;
            }
        } else {
            /* ── plain file arg: use directly ── */
            strncpy(resolved_image, arg, MAX_PATH_LEN - 1);
            have_image = 1;
        }
    } else {
        /* ── no arg: Patch-1 from config image_path ── */
        if (cfg.image_path[0] == '\0') {
            fprintf(stderr,
                    "Error: no image_path in config and no argument given.\n");
            fprintf(stderr, "Usage: %s [FILE|DIR]\n", argv[0]);
            fprintf(stderr,
                    "Or set image_path in ~/.config/anifetch/config.conf\n");
            return 1;
        }

        /* strip filename from config path to get the directory */
        char dir_path[MAX_PATH_LEN];
        strncpy(dir_path, cfg.image_path, MAX_PATH_LEN - 1);
        dir_path[MAX_PATH_LEN - 1] = '\0';

        char *slash = strrchr(dir_path, '/');
        if (slash) {
            *slash = '\0'; /* dir_path is now just the directory */
        } else {
            /* config path has no slash; treat it as current dir */
            strncpy(dir_path, ".", sizeof(dir_path));
        }

        have_image = pick_random_image_from_dir(dir_path, resolved_image,
                                                sizeof(resolved_image));
        if (!have_image) {
            /* fallback: use the exact path from config */
            strncpy(resolved_image, cfg.image_path, MAX_PATH_LEN - 1);
            have_image = 1;
        }
    }

    if (!have_image || resolved_image[0] == '\0') {
        fprintf(stderr, "Error: could not resolve an image path.\n");
        return 1;
    }

    /* ── sanity-check the image file ── */
    if (access(resolved_image, F_OK) != 0) {
        fprintf(stderr, "Error: image file '%s' not found\n", resolved_image);
        return 1;
    }

    unsigned char *img_data = NULL;
    size_t img_len = 0;
    int encrypted = ends_with(resolved_image, ".gpg");

    if (encrypted) {
        char pass[128];
        if (!read_passphrase(pass, sizeof(pass)))
            return 1;
        int ok = decrypt_to_memory(resolved_image, pass, &img_data, &img_len);
        secure_wipe(pass, sizeof(pass));
        if (!ok) {
            fprintf(stderr,
                    "Decryption failed (wrong passphrase or bad file).\n");
            return 1;
        }
    }

    /* ── run fetch backend ── */
    char fetch_lines[MAX_LINES][MAX_LINE_LEN];
    int fetch_line_count;

    if (strcmp(cfg.backend, "neofetch") == 0) {
        fetch_line_count = run_neofetch(fetch_lines, MAX_LINES);
    } else {
        fetch_line_count = run_fastfetch(fetch_lines, MAX_LINES);
    }

    if (fetch_line_count < 0) {
        fprintf(stderr, "Error: could not run '%s'\n", cfg.backend);
        return 1;
    }

    /* ── determine placement height ── */
    int place_h = (cfg.img_height > 0) ? cfg.img_height : fetch_line_count;

    /* ── render ── */
    clear_screen();
    if (encrypted) {
        run_icat_mem(&cfg, img_data, img_len, place_h);
        secure_wipe(img_data, img_len);
        free(img_data);
    } else {
        run_icat(&cfg, resolved_image, place_h);
    }

    /* move cursor and print fetch lines to the right of the image */
    for (int i = 0; i < fetch_line_count; i++) {
        printf("\033[%d;%dH", i + 1, cfg.img_width + cfg.text_offset);
        printf("%s", fetch_lines[i]);
    }

    /* park cursor below everything */
    int end_row = (fetch_line_count > place_h ? fetch_line_count : place_h) + 1;
    printf("\033[%d;1H", end_row);
    fflush(stdout);

    return 0;
}
