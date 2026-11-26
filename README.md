# winpad-x11

Ultra-lightweight **Windows 10/11 Precision Touchpad** gestures for **Linux X11 / XFCE** written in pure C.

Zero Python/Ruby runtimes, zero `xdotool` subprocess spawning, **31 KB** stripped binaries, **~3 MB RSS**, and **0.0% idle CPU**.

## Why `winpad-x11`?

On many X11 desktops (especially Xubuntu / XFCE 4.16 with `xserver-xorg-input-libinput < 1.3`), 3-finger and 4-finger touchpad gestures are ignored out of the box, and Ubuntu's default `apt install touchegg` installs the legacy 1.x uTouch package that does not work with `libinput`.

| Project | Language / Stack | Runtime Deps | Windows 3-State Up/Down | Interactive Alt+Tab Scrub | 2-Finger Back/Forward |
|---|---|---|---|---|---|
| **winpad-x11** | **Pure C (`evdev` + `X11`/`XTest`, ~3 MB RSS, 31 KB binaries)** | **None** | **Yes** | **Yes (XKB layout-safe)** | **Yes (app-aware)** |
| Touchégg v2 | C++ daemon + X11 client | PPA / libinput / pugixml / GTK | No | Partial | No |
| libinput-gestures | Python + shell wrapper | `libinput-tools`, `xdotool` | No | No | No |
| Fusuma | Ruby gem | Ruby, `libinput-tools`, `xdotool` | No | No | No |
| xgestured / gebaar | C / C++ daemon | `libinput-dev`, `xdotool` | No | No | No |

## Gestures Included

| Gesture | Action |
|---|---|
| **1-finger tap / drag** | Left click & tap-and-drag (handled natively via `libinput`) |
| **2-finger tap / scroll** | Right click & Natural Scrolling (handled natively via `libinput`) |
| **2-finger horizontal flick** | Browser / File Manager **Back & Forward** (`XF86Back` / `XF86Forward`; `Alt+Left`/`Right` in IDEs; disabled in terminals; locked out during vertical scroll) |
| **3-finger swipe Left / Right** | **Interactive `Alt+Tab` scrubbing** — holds `Alt` while sliding between windows, releases on lift (preserves multi-language XKB layouts like `grp:alt_shift_toggle`) |
| **3-finger swipe Up / Down** | **Exact Windows 10/11 3-state cycle:** `Task View` ↔ `Normal Windows` ↔ `Show Desktop` (swiping Up after Show Desktop restores all windows; swiping Down while Task View is open closes Task View) |
| **4-finger swipe Left / Right** | Switch Virtual Desktops / Workspaces (`_NET_CURRENT_DESKTOP`, falls back to `Alt+Tab` on single-workspace setups) |
| **4-finger swipe Up / Down** | Same `Task View` ↔ `Normal` ↔ `Show Desktop` cycle |

## Requirements

- Linux X11 session (`xfwm4` / EWMH-compatible window manager) with `libinput` (`xserver-xorg-input-libinput`)
- Build dependencies (only needed during compile):
  ```bash
  sudo apt update && sudo apt install -y gcc pkg-config libx11-dev libxtst-dev libgtk-3-dev libwnck-3-dev
  ```

## Quick Install (Xubuntu / XFCE / X11)

```bash
git clone https://github.com/mudmini009/winpad-x11.git
cd winpad-x11
./scripts/setup.sh
```

`scripts/setup.sh` will:
1. Build and strip `winpad-gestures` and `winpad-taskview` into `~/.local/bin/` and install the XFCE autostart entry into `~/.config/autostart/`.
2. Install `/etc/udev/rules.d/65-touchpad-uaccess.rules` (`TAG+="uaccess"` for `ID_INPUT_TOUCHPAD==1`) so the active local user can read touchpad multitouch events immediately **without rebooting or logging out**.
3. Configure `/etc/X11/xorg.conf.d/40-libinput.conf` and XFCE (`xfwm4` + `pointers`) for tap-to-click, `clickfinger`, natural scrolling, and 4 workspaces.
4. Start `winpad-gestures` immediately.

## Manual Build

```bash
make install
```

Check touchpad detection:
```bash
~/.local/bin/winpad-gestures --check
```

## Configuration

Edit `~/.config/winpad-gestures.conf` and reload live without restarting:

```bash
killall -HUP winpad-gestures
```

Main options in `~/.config/winpad-gestures.conf`:
- `two_finger_swipe = natural | arrow | off`
- `three_finger_horiz = alt_tab_scrub | alt_tab_once | off`
- `four_finger_horiz = workspace_natural | workspace_direct | alt_tab | off`
- `taskview_cmd = ~/.local/bin/winpad-taskview` (can be swapped for `xfdashboard` or `rofi -show window`)

## Troubleshooting

- **Permission denied on `/dev/input/event*`:** Re-run `./scripts/setup.sh` (or verify `getfacl /dev/input/event*` shows your username).
- **Check live event/jump logs:**
  ```bash
  pkill -x winpad-gestures && ~/.local/bin/winpad-gestures --debug
  ```
- **Older X11 apps dropping first scroll tick after reversing direction (`xserver-xorg-input-libinput 1.2.x`):**
  ```bash
  xinput set-prop "<Your Touchpad Name>" "libinput High Resolution Wheel Scroll Enabled" 0
  ```

## License

MIT — see [LICENSE](LICENSE).
