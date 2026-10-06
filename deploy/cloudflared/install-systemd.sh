#!/usr/bin/env bash
set -euo pipefail

service_user=kubik-cloudflared
unit_name=kubik-cloudflared.service
state_dir=/etc/kubik-cloudflared
token_file="$state_dir/token"
unit_file="/etc/systemd/system/$unit_name"

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

require_systemd() {
    [[ "$(uname -s)" == Linux ]] || fail "This installer supports Linux with systemd only."
    (( EUID == 0 )) || fail "Run with sudo: sudo bash deploy/cloudflared/install-systemd.sh"
    [[ -d /run/systemd/system ]] || fail "systemd is not running as the system manager."
}

require_commands() {
    local command_name
    for command_name in cloudflared systemctl useradd userdel getent nologin mktemp chown chmod rm rmdir mkdir sed grep mv ln; do
        command -v "$command_name" >/dev/null 2>&1 || fail "Required command not found: $command_name"
    done
}

require_cloudflared() {
    local version_output version version_year version_minor
    cloudflared_bin="$(command -v cloudflared)"
    [[ "$cloudflared_bin" =~ ^/[A-Za-z0-9_./+-]+$ ]] || fail "Install cloudflared at an absolute path without spaces or special characters."
    version_output="$("$cloudflared_bin" --version 2>&1)" || fail "Could not read the cloudflared version."
    version="$(sed -nE 's/.*version ([0-9]{4})\.([0-9]+)\.[0-9]+.*/\1.\2/p' <<< "$version_output")"
    [[ -n "$version" ]] || fail "Could not parse the cloudflared version: $version_output"
    IFS=. read -r version_year version_minor <<< "$version"
    if (( 10#$version_year < 2025 || (10#$version_year == 2025 && 10#$version_minor < 4) )); then
        fail "cloudflared 2025.4.0 or later is required for --token-file; found $version."
    fi
}

require_fresh_install() {
    systemctl show -p LoadState --value "$unit_name" 2>/dev/null | grep -qx not-found \
        || fail "$unit_name already exists or systemd could not confirm that it is unused."
    getent passwd "$service_user" >/dev/null 2>&1 \
        && fail "The $service_user account already exists; review it before continuing."
    [[ ! -e "$state_dir" && ! -L "$state_dir" ]] || fail "$state_dir already exists; review it before continuing."
    [[ ! -e "$unit_file" && ! -L "$unit_file" ]] || fail "$unit_file already exists; review it before continuing."
}

collect_token() {
    [[ -r /dev/tty ]] || fail "Run from an interactive terminal so the tunnel token can be entered without echo."
    printf 'Paste the remotely-managed Tunnel token (input is hidden): ' >/dev/tty
    IFS= read -r -s tunnel_token </dev/tty || fail "Could not read the tunnel token."
    printf '\n' >/dev/tty
    [[ -n "$tunnel_token" && ! "$tunnel_token" =~ [[:space:]] ]] || fail "The tunnel token must be one non-empty line."
}

ignore_failure() {
    "$@" >/dev/null 2>&1 || return 0
}

require_systemd
require_commands
require_cloudflared
require_fresh_install
collect_token

created_user=0
created_state_dir=0
created_unit=0
completed=0
tmp_token=''
tmp_unit=''
cleanup() {
    result=$?
    trap - EXIT
    if (( completed == 0 )); then
        if (( created_unit == 1 )); then
            ignore_failure systemctl disable --now "$unit_name"
            ignore_failure rm -f -- "$unit_file"
            ignore_failure systemctl daemon-reload
        fi
        if [[ -n "$tmp_unit" ]]; then
            ignore_failure rm -f -- "$tmp_unit"
        fi
        if [[ -n "$tmp_token" ]]; then
            ignore_failure rm -f -- "$tmp_token"
        fi
        if (( created_state_dir == 1 )); then
            ignore_failure rm -f -- "$token_file"
            ignore_failure rmdir -- "$state_dir"
        fi
        if (( created_user == 1 )); then
            ignore_failure userdel "$service_user"
        fi
    fi
    exit "$result"
}
trap cleanup EXIT

useradd --system --user-group --no-create-home --home-dir /nonexistent \
    --shell "$(command -v nologin)" "$service_user"
created_user=1
mkdir -m 0755 -- "$state_dir"
created_state_dir=1
chown root:root "$state_dir"
chmod 0755 "$state_dir"
tmp_token="$(mktemp "$state_dir/.token.XXXXXX")"
printf '%s\n' "$tunnel_token" > "$tmp_token"
unset tunnel_token
chown "$service_user:$service_user" "$tmp_token"
chmod 0600 "$tmp_token"
mv -- "$tmp_token" "$token_file"
tmp_token=''

tmp_unit="$(mktemp "/etc/systemd/system/.kubik-cloudflared.XXXXXX")"
cat > "$tmp_unit" <<UNIT
[Unit]
Description=Kubik Cloudflare Tunnel
Wants=network-online.target
After=network-online.target
StartLimitIntervalSec=0

[Service]
Type=simple
User=$service_user
Group=$service_user
ExecStart=$cloudflared_bin tunnel run --token-file $token_file
Restart=on-failure
RestartSec=10s
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true

[Install]
WantedBy=multi-user.target
UNIT
chown root:root "$tmp_unit"
chmod 0644 "$tmp_unit"
ln -- "$tmp_unit" "$unit_file"
created_unit=1
rm -f -- "$tmp_unit"
tmp_unit=''

systemctl daemon-reload
systemctl enable --now "$unit_name"
systemctl is-active --quiet "$unit_name" || fail "$unit_name did not remain active; inspect it with systemctl status $unit_name."
completed=1
printf 'Installed and started %s. Check connector health in the Cloudflare dashboard.\n' "$unit_name"
