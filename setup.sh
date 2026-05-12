#!/usr/bin/env bash
# =============================================================================
# ScratchOnNDS — Debian 13 (Trixie) setup script
#
# What this does:
#   1. Installs system build dependencies (cmake, git, python3, etc.)
#   2. Installs devkitPro pacman (dkp-pacman) from the official .deb
#   3. Installs nds-dev (devkitARM + libnds + libfat + maxmod)
#   4. Sets persistent environment variables in ~/.bashrc and ~/.profile
#   5. Verifies the toolchain is working
#
# Usage:
#   chmod +x setup.sh
#   ./setup.sh
#
# Run as a normal user with sudo access — do NOT run as root.
# =============================================================================

set -euo pipefail

# ─── Colours ─────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'
CYN='\033[0;36m'; BLD='\033[1m'; RST='\033[0m'

info()  { echo -e "${CYN}[INFO]${RST}  $*"; }
ok()    { echo -e "${GRN}[ OK ]${RST}  $*"; }
warn()  { echo -e "${YLW}[WARN]${RST}  $*"; }
die()   { echo -e "${RED}[FAIL]${RST}  $*" >&2; exit 1; }
header(){ echo -e "\n${BLD}${CYN}══════════════════════════════════════${RST}"; \
          echo -e "${BLD}${CYN}  $*${RST}"; \
          echo -e "${BLD}${CYN}══════════════════════════════════════${RST}\n"; }

# ─── Sanity checks ───────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]] && die "Do not run this script as root. Run as a normal user with sudo."

command -v sudo >/dev/null 2>&1 || die "sudo is required but not installed."
command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || \
    die "curl or wget is required but neither is installed."

export DEBIAN_FRONTEND=noninteractive

# Prefer curl, fall back to wget
if command -v curl >/dev/null 2>&1; then
    download() { curl -fsSL -o "$2" "$1"; }
else
    download() { wget -q -O "$2" "$1"; }
fi

# ─── Configuration ────────────────────────────────────────────────────────────
DEVKITPRO_ROOT="/opt/devkitpro"
DEVKITARM_ROOT="${DEVKITPRO_ROOT}/devkitARM"

ENV_BLOCK='
# ── devkitPro environment (added by ScratchOnNDS setup.sh) ──
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=${DEVKITPRO}/tools/bin:${DEVKITARM}/bin:${PATH}
# ────────────────────────────────────────────────────────────'

# ─── Step 1: System dependencies ─────────────────────────────────────────────
header "Step 1/5 — System dependencies"

PACKAGES=(
    # Core build tools
    build-essential
    cmake
    make
    git
    pkg-config

    # Python (for validate_sb3.py / check_project_json.py)
    python3
    python3-pip

    # Runtime / link deps that devkitPro tools need
    libncurses6
    libncurses-dev
    zlib1g
    zlib1g-dev

    # Needed to install the .deb
    gdebi-core
)

info "Updating apt package lists..."
sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq

info "Installing: ${PACKAGES[*]}"
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${PACKAGES[@]}"
ok "System packages installed."

# ─── Step 2: devkitPro pacman ─────────────────────────────────────────────────
header "Step 2/5 — devkitPro pacman"

LOCAL_INSTALLER="./devkitproinstaller.sh"

if command -v dkp-pacman >/dev/null 2>&1; then
    INSTALLED_VER=$(dkp-pacman --version 2>/dev/null | head -1 || echo "unknown")
    ok "dkp-pacman already installed (${INSTALLED_VER}). Skipping install."
else
    [[ -f "${LOCAL_INSTALLER}" ]] || \
        die "Missing ${LOCAL_INSTALLER}"

    chmod +x "${LOCAL_INSTALLER}"

    info "Running local devkitPro installer..."

    # Prevent installer oddities from aborting setup
    if ! sudo DEBIAN_FRONTEND=noninteractive bash "${LOCAL_INSTALLER}" < /dev/null; then
        warn "Installer returned non-zero status."
        warn "Checking whether dkp-pacman was installed anyway..."
    fi

    if command -v dkp-pacman >/dev/null 2>&1; then
        ok "dkp-pacman installed successfully."
    else
        die "devkitPro installer failed."
    fi
fi

# ─── Step 3: Environment variables ───────────────────────────────────────────
header "Step 3/5 — Environment variables"

add_env_to_file() {
    local file="$1"
    if grep -q "devkitPro environment" "$file" 2>/dev/null; then
        info "devkitPro env block already present in ${file}. Skipping."
    else
        echo -e "${ENV_BLOCK}" >> "$file"
        ok "Added devkitPro env block to ${file}."
    fi
}

add_env_to_file "${HOME}/.bashrc"
add_env_to_file "${HOME}/.profile"

# Also export for the current shell session so the rest of this script works
export DEVKITPRO="${DEVKITPRO_ROOT}"
export DEVKITARM="${DEVKITARM_ROOT}"
export DEVKITPPC="${DEVKITPRO_ROOT}/devkitPPC"
export PATH="${DEVKITPRO_ROOT}/tools/bin:${DEVKITARM_ROOT}/bin:${PATH}"

# Some devkitPro packages also install environment helpers in /etc/profile.d.
# Source them now so verification checks can see ndstool and other tools.
if [[ -f /etc/profile.d/devkit-env.sh ]]; then
    # shellcheck source=/etc/profile.d/devkit-env.sh
    source /etc/profile.d/devkit-env.sh
fi

# ─── Step 4: NDS toolchain and libraries ─────────────────────────────────────
header "Step 4/5 — NDS development packages (nds-dev)"

info "Syncing dkp-pacman package database..."
sudo dkp-pacman -Sy --noconfirm

info "Installing nds-dev (devkitARM + libnds + libfat + maxmod + ds tools)..."
# nds-dev is a package group that pulls in everything needed to build .nds ROMs
printf "\n" | sudo dkp-pacman -S --noconfirm --needed \
    nds-dev \
    devkitARM

ok "NDS toolchain installed."

# ─── Step 5: Verify ──────────────────────────────────────────────────────────
header "Step 5/5 — Verification"

CHECKS_PASSED=0
CHECKS_TOTAL=4

check() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        ok "${desc}"
        CHECKS_PASSED=$((CHECKS_PASSED + 1))
    else
        warn "${desc} — NOT FOUND (command: $*)"
    fi
}

check "arm-none-eabi-gcc"      arm-none-eabi-gcc --version
check "arm-none-eabi-g++"      arm-none-eabi-g++ --version
check "ndstool"                ndstool --version
check "libnds headers"         test -f "${DEVKITPRO_ROOT}/libnds/include/nds.h"

echo ""
if [[ $CHECKS_PASSED -eq $CHECKS_TOTAL ]]; then
    ok "All ${CHECKS_TOTAL}/${CHECKS_TOTAL} checks passed."
else
    warn "${CHECKS_PASSED}/${CHECKS_TOTAL} checks passed."
    warn "Some tools may not be on PATH yet — try opening a new terminal or running:"
    warn "  source ~/.bashrc"
fi

# ─── Done ─────────────────────────────────────────────────────────────────────
header "Setup complete!"
echo -e "  ${BLD}Next steps:${RST}"
echo ""
echo -e "  1. Open a new terminal (or run ${CYN}source ~/.bashrc${RST})"
echo -e "     to pick up the devkitPro environment variables."
echo ""
echo -e "  2. Clone the repo (if you haven't already):"
echo -e "     ${CYN}git clone https://github.com/fsminecrafter/ScratchOnNDS.git${RST}"
echo -e "     ${CYN}cd ScratchOnNDS${RST}"
echo ""
echo -e "  3. Build the .nds ROM:"
echo -e "     ${CYN}make${RST}"
echo ""
echo -e "  4. Run host-side unit tests (no DS hardware needed):"
echo -e "     ${CYN}cmake -B build -DSCRATCHDS_HOST_TESTS=ON${RST}"
echo -e "     ${CYN}cmake --build build${RST}"
echo -e "     ${CYN}ctest --test-dir build${RST}"
echo ""
echo -e "  5. Validate a project before copying to SD card:"
echo -e "     ${CYN}python3 tools/validate_sb3.py path/to/project.sb3${RST}"
echo ""