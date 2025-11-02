# anifetch

A lightweight C wrapper for neofetch that uses a static kitten binary to render images via the Kitty graphics protocol.

**Why:** Unlike neofetch --backend kitty, which requires the full Kitty installation, this works even in terminals like Ghostty with only the standalone kitten binary present.

![Demo](https://via.placeholder.com/800x400.png?text=Your+Screenshot+Here)

## Features

- 🖼️ **Side-by-side rendering** - Image on the left, system info on the right
- ⚙️ **Fully configurable** - Customize image size, spacing, crop mode via config file
- 🚀 **Fast** - Written in C for minimal overhead
- 🎨 **Crop modes** - Choose between `auto` (preserve aspect ratio) or `fill` (crop to fit)
- 🖥️ **Terminal agnostic** - Works with any terminal supporting Kitty graphics protocol (Kitty, WezTerm, Ghostty)

## Prerequisites

- A terminal that supports the Kitty graphics protocol:
  - [Kitty](https://sw.kovidgoyal.net/kitty/)
  - [WezTerm](https://wezfurlong.org/wezterm/)
  - [Ghostty](https://ghostty.org/)
- `neofetch` installed on your system
- GCC compiler
- `kitten` static binary (see installation steps)

## Installation

### 1. Download the `kitten` static binary

Download the latest nightly build:

```bash
sudo curl -L https://github.com/kovidgoyal/kitty/releases/download/nightly/kitten-linux-amd64 \
     -o /usr/local/bin/kitten
sudo chmod +x /usr/local/bin/kitten
```

**Note:** This static binary works standalone and doesn't require Kitty to be installed.

### 2. Compile the program

```bash
gcc -O3 -o anifetch main.c
sudo mv anifetch /usr/local/bin/anifetch
```

### 3. Create the config directory

```bash
mkdir -p ~/.config/anifetch
```

### 4. Create the configuration file

Create `~/.config/anifetch/config.conf`:

```ini
# Image width in characters
img_width=40

# Space between image and text (in columns)
text_offset=5

# Crop mode: "auto" (preserve aspect ratio) or "fill" (crop to fit)
crop_mode=fill

# Default image path (tilde expansion supported!)
image_path=~/Pictures/anime.jpg
```

## Usage

### Direct usage

If you know your terminal supports the Kitty graphics protocol:

```bash
anifetch ~/path/to/image.jpg
```

Or use the default image from config:

```bash
anifetch
```

### Recommended: Shell integration

Add this to your `~/.zshrc` (or `~/.bashrc`):

```bash
# To bypass tmux messing around
if [ -z "$REAL_TERM" ]; then
    case "$TERM" in
        *kitty*)   export REAL_TERM="kitty" ;;
        *wezterm*) export REAL_TERM="wezterm" ;;
        *ghostty*) export REAL_TERM="ghostty" ;;
        *)         export REAL_TERM="generic" ;;
    esac
fi

anime() {
    case "$REAL_TERM" in
        kitty|xterm-kitty|wezterm|xterm-ghostty|ghostty)
            # Lightweight C wrapper around neofetch leveraging a static kitten binary
            /usr/local/bin/anifetch

            # NOTE: The following command only works if Kitty is installed.
            # INFO: It will work in Ghostty or Kitty as long as the Kitty app is installed,
            #       but will fail once Kitty is removed, even if the kitten binary is present.
            # neofetch --backend kitty --source "$HOME/.config/neofetch/image/makeine.webp" \
            #          --size 400px --crop_mode fit
            ;;
        *)
            # Fallback: Your terminal doesn't support image rendering
            clear
            neofetch
            ;;
    esac
}
```

Then simply run:

```bash
anime
```

### Optional: Run on shell startup

Add this to your `~/.zshrc`:

```bash
anime # OR `anifetch`
```

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `img_width` | integer | `40` | Width of the image in terminal columns |
| `text_offset` | integer | `5` | Space between image and neofetch text |
| `crop_mode` | string | `auto` | Image crop mode: `auto` or `fill` |
| `image_path` | string | none | Default image path (supports `~/`) |

### Crop Modes

- **`auto`**: Preserves the original aspect ratio of the image. Best for portraits or when you want to see the full image.
- **`fill`**: Crops the image to fill the entire allocated space (centered crop). Best for making images appear consistently sized.

## Customizing Neofetch Output

You can customize what information neofetch displays. For reference configurations, check out:
- [jelius-sama's neofetch config](https://github.com/jelius-sama/dotfiles/tree/neofetch)

Edit your `~/.config/neofetch/config.conf` to customize the information displayed.

## Troubleshooting

### Image not displaying

1. **Check terminal compatibility**: Verify your terminal supports Kitty graphics protocol
2. **Verify kitten binary**: Run `kitten icat /path/to/image.jpg` directly
3. **Check image path**: Ensure the image file exists and is readable

### Gap between output and prompt

This is normal - the image height determines the spacing. Adjust `img_width` in config to change the image size.

### Tmux issues

The shell integration above handles tmux by detecting the real terminal type before tmux wraps it. Make sure `REAL_TERM` is exported before entering tmux.

### Config not loading

- Verify config file exists: `cat ~/.config/anifetch/config.conf`
- Check file permissions: `ls -la ~/.config/anifetch/config.conf`
- Look for error messages when running the program

## How It Works

1. Reads configuration from `~/.config/anifetch/config.conf`
2. Runs `neofetch --off` to get system info without ASCII art
3. Clears the screen
4. Uses `kitten icat` with the `--place` option to render the image on the left
5. Positions cursor and prints neofetch output line-by-line on the right
6. Positions cursor after the content with no extra gaps

## Credits

- Uses the [Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/)
- Inspired by [neofetch](https://github.com/dylanaraps/neofetch)
- Terminal compatibility layer idea from various dotfiles in the community

## License

[MIT License](./LICENSE)

## Contributing

Pull requests are welcome! Feel free to:
- Add new configuration options
- Improve terminal compatibility
- Fix bugs
- Enhance documentation

---

Made with ❤️ for anime and ricing enthusiasts
