#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TARGET_USER="${SUDO_USER:-$USER}"
TARGET_HOME="$(eval echo "~$TARGET_USER")"

run_as_user() {
    if [ "$(id -u)" -eq 0 ] && [ "$TARGET_USER" != "root" ]; then
        su -s /bin/bash "$TARGET_USER" -c "DISPLAY=${DISPLAY:-:0.0} DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u "$TARGET_USER")/bus} $*"
    else
        bash -c "$*"
    fi
}

echo "=== winpad-x11 Setup for X11/XFCE (user: $TARGET_USER) ==="

# 1. Build & install binaries/configs if running from the repository
if [ -f "$REPO_ROOT/Makefile" ] && [ -f "$REPO_ROOT/src/winpad-gestures.c" ]; then
    echo "[1/5] Building and installing winpad-gestures & winpad-taskview..."
    run_as_user "make -C '$REPO_ROOT' install"
else
    echo "[1/5] Installing from user source directory..."
    run_as_user "mkdir -p '$TARGET_HOME/.local/bin'"
    if [ ! -x "$TARGET_HOME/.local/bin/winpad-gestures" ] && [ -f "$TARGET_HOME/.local/src/winpad-gestures.c" ]; then
        run_as_user "gcc -O2 -flto -Wall -Wextra '$TARGET_HOME/.local/src/winpad-gestures.c' -o '$TARGET_HOME/.local/bin/winpad-gestures' -lX11 -lXtst -lm && strip --strip-unneeded '$TARGET_HOME/.local/bin/winpad-gestures'"
    fi
    if [ ! -x "$TARGET_HOME/.local/bin/winpad-taskview" ] && [ -f "$TARGET_HOME/.local/src/winpad-taskview.c" ]; then
        if pkg-config --exists xcomposite 2>/dev/null; then
            run_as_user "gcc -O2 -flto -Wall -Wextra -DHAVE_XCOMPOSITE '$TARGET_HOME/.local/src/winpad-taskview.c' -o '$TARGET_HOME/.local/bin/winpad-taskview' \$(pkg-config --cflags --libs gtk+-3.0 libwnck-3.0 xcomposite x11) -lm && strip --strip-unneeded '$TARGET_HOME/.local/bin/winpad-taskview'"
        else
            run_as_user "gcc -O2 -flto -Wall -Wextra '$TARGET_HOME/.local/src/winpad-taskview.c' -o '$TARGET_HOME/.local/bin/winpad-taskview' \$(pkg-config --cflags --libs gtk+-3.0 libwnck-3.0) -lm && strip --strip-unneeded '$TARGET_HOME/.local/bin/winpad-taskview'"
        fi
    fi
fi

# 2. Grant active local desktop user read access to touchpad input events (without logging out)
UDEV_RULE="/etc/udev/rules.d/65-touchpad-uaccess.rules"
echo "[2/5] Installing udev touchpad uaccess rule: $UDEV_RULE"
sudo tee "$UDEV_RULE" >/dev/null <<'EOF'
ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event*", ENV{ID_INPUT_TOUCHPAD}=="1", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input
sudo udevadm settle --timeout=5 || true

# 3. Configure generic libinput Precision Touchpad Xorg defaults
XORG_CONF="/etc/X11/xorg.conf.d/40-libinput.conf"
echo "[3/5] Updating $XORG_CONF with generic libinput Precision Touchpad defaults"
sudo tee "$XORG_CONF" >/dev/null <<'EOF'
Section "InputClass"
    Identifier "Windows-Style Precision Touchpad"
    MatchIsTouchpad "on"
    MatchDevicePath "/dev/input/event*"
    Driver "libinput"
    Option "Tapping" "on"
    Option "TappingDrag" "on"
    Option "TappingButtonMap" "lrm"
    Option "ClickMethod" "clickfinger"
    Option "ScrollMethod" "twofinger"
    Option "NaturalScrolling" "true"
    Option "DisableWhileTyping" "true"
    Option "HorizontalScrolling" "true"
EndSection
EOF

# 4. Apply XFCE settings as the desktop user
echo "[4/5] Applying XFCE touchpad and workspace defaults..."
run_as_user '
if command -v xfconf-query >/dev/null 2>&1; then
    xfconf-query -c xfwm4 -p /general/mousewheel_rollup -s false 2>/dev/null || true
    xfconf-query -c xfwm4 -p /general/scroll_workspaces -s false 2>/dev/null || true
    xfconf-query -c xfwm4 -p /general/workspace_count -s 4 2>/dev/null || true
    xfconf-query -c xfwm4 -p /general/cycle_minimized -s true 2>/dev/null || true
    for ch in $(xfconf-query -c pointers -l | sed -n "s|^\(/[^/]*_Touchpad\)/.*|\1|p" | sort -u); do
        xfconf-query -c pointers -p "$ch/ReverseScrolling" -n -t bool -s true 2>/dev/null || true
        xfconf-query -c pointers -p "$ch/Properties/libinput_Tapping_Enabled" -n -t int -s 1 2>/dev/null || true
        xfconf-query -c pointers -p "$ch/Properties/libinput_Click_Method_Enabled" -n -t int -s 0 -t int -s 1 2>/dev/null || true
    done
    for ch in $(xfconf-query -c pointers -l | sed -n "s|^\(/ASUP[^/]*_Mouse\)/.*|\1|p" | sort -u); do
        xfconf-query -c pointers -p "$ch/ReverseScrolling" -n -t bool -s true 2>/dev/null || true
    done
fi
'

# 5. Verify touchpad access and restart daemon
echo "[5/5] Verifying touchpad & starting winpad-gestures..."
run_as_user "'$TARGET_HOME/.local/bin/winpad-gestures' --check"
run_as_user "pkill -x winpad-gestures 2>/dev/null || true; for _ in 1 2 3 4 5 6 7 8; do pgrep -x winpad-gestures >/dev/null || break; sleep 0.1; done; DISPLAY=${DISPLAY:-:0.0} setsid -f '$TARGET_HOME/.local/bin/winpad-gestures' </dev/null >/tmp/winpad-gestures.log 2>&1"
sleep 0.5
pgrep -a winpad-gestures

echo "Done! Windows-style touchpad gestures are active and set to autostart on login."
