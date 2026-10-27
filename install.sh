#!/usr/bin/env bash
# Install or update Columbia Street on a TrimUI Brick, Brick Hammer or Brick Pro. Run it on a
# computer on the same network as the TrimUI:
#
#   curl -fsSL https://raw.githubusercontent.com/sydilaxe/Trimui-Columbiastreet-dev/main/install.sh | bash
#
# It finds the TrimUI over SSH (the firmware's SSH server must be on), compares the installed
# version with the latest GitHub release, and copies the new build to SD:/Apps/ColumbiaStreet if
# the release is newer. Your song (columbia.sav) and any breaks or basses you added stay as they
# are.
#
#   CS_HOST=192.168.1.28    skip the scan and use this address
#   CS_USER=root            SSH user (root by default)
#   CS_DEVICE=brickpro      the Brick Pro's build (with the sticks); brick for the Brick and Brick
#                           Hammer. An installed copy says which it is, so this is only needed once.
#   CS_FORCE=1              reinstall even if the TrimUI already has the latest version
#
# The address that worked is saved in ~/.config/columbiastreet/host and tried first next time.

set -euo pipefail

REPO=sydilaxe/Trimui-Columbiastreet-dev
APPS=/mnt/SDCARD/Apps
SSH_USER=${CS_USER:-root}
SAVED=${XDG_CONFIG_HOME:-$HOME/.config}/columbiastreet/host

say() { printf 'columbia: %s\n' "$*" >&2; }
die() { say "$*"; exit 1; }

# print "IP BANNER" if IP answers on port 22 with an SSH banner
probe() {
    local banner=""
    { exec 3<>"/dev/tcp/$1/22"; } 2>/dev/null || return 0
    IFS= read -r -t 2 banner <&3 || true
    exec 3<&-
    [[ $banner == SSH-* ]] && printf '%s %s\n' "$1" "${banner%$'\r'}"
    return 0
}

# probe one address, giving up after 3 s (a host on another network can take minutes to time out)
probe_quick() {
    local i pid
    probe "$1" >"$tmp/one" &
    pid=$!
    for i in $(seq 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    { kill "$pid" || true; wait "$pid" || true; } 2>/dev/null
    cat "$tmp/one"
}

local_ip() {
    if command -v ip >/dev/null; then
        ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p'
    else
        local ifc
        ifc=$(route -n get default 2>/dev/null | awk '/interface:/ {print $2}')
        [ -n "$ifc" ] && ipconfig getifaddr "$ifc"
    fi
}

# probe every address on this machine's /24 at once; the handhelds' sshd is usually dropbear,
# which sets them apart from the OpenSSH hosts on a typical home network (any SSH host is
# offered when no dropbear answers)
scan() {
    local me prefix i line found=()
    me=$(local_ip || true)
    [ -n "$me" ] || die "can't work out this computer's LAN address; set CS_HOST=<trimui ip>"
    prefix=${me%.*}
    say "looking for the TrimUI on $prefix.0/24..."
    for i in $(seq 1 254); do
        [ "$prefix.$i" = "$me" ] || probe "$prefix.$i" &
    done >"$tmp/scan"
    sleep 3
    { kill $(jobs -p) || true; wait; } 2>/dev/null
    while read -r line; do found+=("$line"); done < <(grep -i dropbear "$tmp/scan" | sort -t. -k4 -n)
    if [ ${#found[@]} -eq 0 ]; then
        while read -r line; do found+=("$line"); done < <(sort -t. -k4 -n "$tmp/scan")
    fi

    if [ ${#found[@]} -eq 0 ]; then
        say "no SSH server answered on $prefix.0/24."
        say "turn on SSH on the TrimUI, or set CS_HOST=<trimui ip>"
        exit 1
    elif [ ${#found[@]} -eq 1 ]; then
        host=${found[0]%% *}
    else
        say "more than one candidate:"
        for i in "${!found[@]}"; do say "  $((i + 1))) ${found[$i]}"; done
        read -r -p "columbia: which one? " i </dev/tty
        [[ $i =~ ^[0-9]+$ ]] && [ "$i" -ge 1 ] && [ "$i" -le ${#found[@]} ] || die "no such choice"
        host=${found[$((i - 1))]%% *}
    fi
}

# true if dotted version $1 is newer than $2
newer() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        n = split(a, x, "."); m = split(b, y, ".")
        for (i = 1; i <= (n > m ? n : m); i++) {
            if (x[i] + 0 > y[i] + 0) exit 0
            if (x[i] + 0 < y[i] + 0) exit 1
        }
        exit 1 }'
}

on_device() {
    ssh -o ControlMaster=auto -o ControlPath="$tmp/ssh" -o ControlPersist=120 \
        -o StrictHostKeyChecking=accept-new -o HostKeyAlgorithms=+ssh-rsa \
        -o ConnectTimeout=5 "$SSH_USER@$host" "$@"
}

main() {
    local c latest installed device asset

    for c in curl unzip ssh tar awk; do
        command -v "$c" >/dev/null || die "this needs $c"
    done

    tmp=$(mktemp -d)
    trap 'ssh -o ControlPath="$tmp/ssh" -O exit trimui 2>/dev/null || true; rm -rf "$tmp"' EXIT

    latest=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" |
        sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -n 1) || true
    [ -n "$latest" ] || die "couldn't read the latest release from github.com/$REPO"
    say "latest release: v$latest"

    host=${CS_HOST:-}
    if [ -z "$host" ] && [ -s "$SAVED" ]; then
        host=$(cat "$SAVED")
        if [ -n "$(probe_quick "$host")" ]; then
            say "using the saved address $host"
        else
            say "saved address $host isn't answering"
            host=
        fi
    fi
    [ -n "$host" ] || scan

    say "connecting to $SSH_USER@$host"
    # the installed version and build (brick / brickpro), or nothing
    installed=$(on_device "test -d $APPS || exit 3; d=$APPS/ColumbiaStreet;
        echo \"\$(cat \$d/VERSION 2>/dev/null) \$(cat \$d/DEVICE 2>/dev/null)\"" </dev/null) ||
        die "couldn't log in to $host, or it has no $APPS (is it the TrimUI card?)"
    mkdir -p "$(dirname "$SAVED")" && echo "$host" >"$SAVED"
    installed=$(printf '%s\n' "$installed" | head -n 1)
    device=${CS_DEVICE:-${installed#* }}
    installed=${installed%% *}
    case $device in
    brick | brickpro) ;;
    "")
        device=brick
        say "installing the Brick / Brick Hammer build (CS_DEVICE=brickpro for the Brick Pro's)"
        ;;
    *) die "CS_DEVICE is brick or brickpro, not $device" ;;
    esac
    asset=ColumbiaStreet-$device.zip

    if [ -z "$installed" ]; then
        say "the TrimUI has no Columbia Street yet"
    elif [ "${CS_FORCE:-}" != 1 ] && ! newer "$latest" "$installed"; then
        say "the TrimUI already has v$installed, nothing to do (CS_FORCE=1 reinstalls)"
        return
    else
        say "the TrimUI has v$installed"
    fi

    if on_device "pidof columbia" </dev/null >/dev/null; then
        die "Columbia Street is running on the TrimUI; quit it (MENU) and run this again"
    fi

    say "downloading v$latest ($asset)..."
    curl -fsSL -o "$tmp/$asset" "https://github.com/$REPO/releases/download/v$latest/$asset"
    unzip -q "$tmp/$asset" -d "$tmp/zip"
    [ -f "$tmp/zip/Apps/ColumbiaStreet/columbia" ] || die "the release zip has no Apps/ColumbiaStreet/columbia"
    # the song is the user's (releases don't carry one, but never overwrite it)
    rm -f "$tmp/zip/Apps/ColumbiaStreet/"columbia.sav*

    say "copying to $host:$APPS/ColumbiaStreet..."
    (cd "$tmp/zip/Apps" && COPYFILE_DISABLE=1 tar --format=ustar -cf - ColumbiaStreet) |
        on_device "tar -xf - -C $APPS && sync"
    say "Columbia Street v$latest is installed. Launch it from Apps."
}

# everything runs from here, so bash has read the whole script before `curl | bash` starts it
main "$@"
