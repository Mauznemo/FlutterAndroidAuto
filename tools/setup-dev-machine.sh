#!/usr/bin/env bash
# One shot provisioning for a Linux dev machine.
#
#   tools/setup-dev-machine.sh --build-deps   native toolchain and libraries
#   tools/setup-dev-machine.sh --udev         let a normal user talk to an Android phone
#   tools/setup-dev-machine.sh --agent-tools  KDE desktop software, only for dev/
#   tools/setup-dev-machine.sh --all         all three, KDE included
#
# Building and running this project needs the first two. --agent-tools is for the
# scripts in dev/, which drive one particular desktop and are not part of the plugin:
# it installs KDE's Spectacle and ydotool, so it pulls KDE onto a machine that may not
# have it. Do not run it unless you want dev/ui.sh to work.
#
# Written for Ubuntu/Debian. All three need root, and say so before they touch anything:
# --build-deps installs packages, --udev writes a rule into /etc/udev/rules.d and adds
# this user to plugdev, and --agent-tools does both of those for ydotool. Checked up
# front rather than discovered halfway through an apt run.

set -euo pipefail

BUILD_DEPS=(
  # ccache is not needed to build, but `flutter clean` throws the whole object tree away
  # and aasdk is most of it. With ccache a clean rebuild is twenty seconds instead of
  # three minutes. The plugin's CMake picks it up on its own if it is installed.
  build-essential cmake ninja-build pkg-config git ccache
  libboost-all-dev libusb-1.0-0-dev libssl-dev
  libprotobuf-dev protobuf-compiler
  libavcodec-dev libavutil-dev libswscale-dev
  libva-dev libva-drm2 vainfo
  libpulse-dev
  libgtk-3-dev libegl1-mesa-dev libgles2-mesa-dev
)

# Only for dev/. kde-spectacle in particular drags in a good deal of KDE.
AGENT_TOOLS=(ydotool kde-spectacle wl-clipboard python3-pil)

# Checked once, before any sub-command runs, rather than left to the first sudo in the
# middle of an apt run. `sudo -v` prompts here, where the caller is expecting it, and
# warms the timestamp so the rest of the run does not stop to ask again.
require_sudo() {
  local why="$1"
  if ! command -v sudo >/dev/null; then
    echo "sudo is not installed, and root is needed to $why." >&2
    exit 1
  fi
  sudo -n true 2>/dev/null && return 0
  if [ -t 0 ]; then
    echo "Root is needed to $why."
    sudo -v && return 0
    echo "Could not get root. Nothing has been installed or changed." >&2
    exit 1
  fi
  echo "Root is needed to $why, and there is no terminal to ask for a password on." >&2
  echo "  Run 'sudo -v' first, then this again. Nothing has been changed." >&2
  exit 1
}

# Flutter's Linux build uses clang, and clang on this distro targets the newest installed
# GCC, which is not necessarily the one `gcc` runs. Without that GCC's libstdc++ headers,
# every C++ compile fails with "'limits' file not found", which looks like a project
# problem and is not. Which version that is depends on the machine, so ask it.
newest_gcc_major() {
  # Clang's own answer, which is the one that decides this. It prints a line like
  # "Selected GCC installation: /usr/lib/gcc/x86_64-linux-gnu/16".
  local selected
  selected="$(clang++ -v -x c++ -E /dev/null 2>&1 |
    sed -n 's|.*Selected GCC installation: .*/\([0-9][0-9]*\)$|\1|p' | tail -n1)"
  if [ -n "$selected" ]; then
    echo "$selected"
    return
  fi
  # No clang yet: the newest of the same directories it would have read.
  local newest
  newest="$(ls -1 /usr/lib/gcc/*/ 2>/dev/null | grep -E '^[0-9]+$' | sort -V | tail -n1)"
  if [ -n "$newest" ]; then
    echo "$newest"
    return
  fi
  gcc -dumpversion 2>/dev/null | cut -d. -f1
}

libstdcxx_dev_package() {
  local major
  major="$(newest_gcc_major)"
  if [ -n "$major" ] && apt-cache show "libstdc++-$major-dev" >/dev/null 2>&1; then
    echo "libstdc++-$major-dev"
    return
  fi
  # Nothing detected, or no package matching what was: the newest the archive offers.
  apt-cache --names-only search '^libstdc\+\+-[0-9]+-dev$' 2>/dev/null |
    cut -d' ' -f1 | sort -V | tail -n1
}

do_build_deps() {
  sudo apt-get update
  sudo apt-get install -y "${BUILD_DEPS[@]}"

  # After the rest, so whatever GCC they pulled in is there to be detected.
  local libstdcxx
  libstdcxx="$(libstdcxx_dev_package)"
  if [ -n "$libstdcxx" ]; then
    sudo apt-get install -y "$libstdcxx"
  else
    echo "warning: found no libstdc++-N-dev package. If C++ compiles fail with" >&2
    echo "'limits' file not found, install the one matching your GCC by hand." >&2
  fi
}

do_agent_tools() {
  sudo apt-get update
  sudo apt-get install -y "${AGENT_TOOLS[@]}"
  # ydotool needs /dev/uinput. Give it to the input group rather than running as root.
  sudo tee /etc/udev/rules.d/60-uinput-ydotool.rules >/dev/null <<'EOF'
KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
EOF
  sudo usermod -aG input "$USER"
  sudo udevadm control --reload-rules
  sudo udevadm trigger --name-match=uinput
  echo "Log out and back in for the 'input' group to take effect,"
  echo "or keep using 'dev/ui.sh setup', which starts ydotoold via sudo."
}

do_udev() {
  # Android phones in AOAP accessory mode re-enumerate as 18d1:2d00..2d05.
  # Without this, opening the device needs root.
  sudo tee /etc/udev/rules.d/99-android-auto.rules >/dev/null <<'EOF'
# Android device, normal mode
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0660", GROUP="plugdev", TAG+="uaccess"
# Android Open Accessory mode, the ids Android Auto shows up as
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d00", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d01", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d04", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d05", MODE="0660", GROUP="plugdev", TAG+="uaccess"
# Samsung, Google, Xiaomi, OnePlus and friends before the accessory switch
SUBSYSTEM=="usb", ATTR{idVendor}=="04e8", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="2717", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="2a70", MODE="0660", GROUP="plugdev", TAG+="uaccess"
EOF
  sudo usermod -aG plugdev "$USER"
  sudo udevadm control --reload-rules
  sudo udevadm trigger
  echo "udev rules installed. Replug the phone."
}

[ $# -gt 0 ] || { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

# Reject unknown options before asking for a password, so a typo costs nothing.
for arg in "$@"; do
  case "$arg" in
    --build-deps|--agent-tools|--udev|--all) ;;
    *) echo "unknown option: $arg" >&2; exit 1 ;;
  esac
done

require_sudo "install packages and write udev rules"

for arg in "$@"; do
  case "$arg" in
    --build-deps)  do_build_deps ;;
    --agent-tools) do_agent_tools ;;
    --udev)        do_udev ;;
    --all)         do_build_deps; do_agent_tools; do_udev ;;
    *) echo "unknown option: $arg" >&2; exit 1 ;;
  esac
done
