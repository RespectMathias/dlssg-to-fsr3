#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script_name="$(basename "${BASH_SOURCE[0]}")"
script_path="$script_dir/$script_name"
target="$script_dir"
mode=""
force=0
uninstall=0
signature_override=0
pause_on_exit=0

if (( $# == 0 )) && [[ -t 0 && -t 1 ]]; then
    pause_on_exit=1
fi

pause_if_interactive() {
    if (( pause_on_exit )); then
        local key=""
        printf '\nPress any key to continue...'
        IFS= read -r -n 1 -s key || true
        printf '\n'
    fi
}

trap pause_if_interactive EXIT

for arg in "$@"; do
    case "$arg" in
        --mode=*) mode="${arg#*=}" ;;
        --target=*) target="${arg#*=}" ;;
        --force) force=1 ;;
        --enable-signature-override) signature_override=1 ;;
        --uninstall) uninstall=1 ;;
        -h|--help)
            echo "Usage: $0 [--mode=version|winhttp|dbghelp|asi|red4ext|nvngx|optiscaler] [--target=PATH] [--force] [--enable-signature-override] [--uninstall]"
            exit 0
            ;;
    esac
done

target="$(cd "$target" 2>/dev/null && pwd || {
    mkdir -p "$target"
    cd "$target"
    pwd
})"

manifest="$target/.dlssg_to_fsr3_install"

hash_file() {
    sha256sum "$1" | cut -d' ' -f1
}

uninstall_files() {
    [[ -f "$manifest" ]] || {
        echo "Install manifest not found: $manifest" >&2
        exit 1
    }

    while IFS='|' read -r kind path hash backup source; do
        case "$kind" in
            move)
                if [[ "$path" != "$source" && -f "$path" ]]; then
                    if [[ "$(hash_file "$path")" == "$hash" ]]; then
                        if [[ -e "$source" ]]; then
                            echo "Warning: original package DLL already exists: $source" >&2
                            continue
                        fi

                        mv "$path" "$source"
                    else
                        echo "Warning: modified installed file kept: $path" >&2
                        continue
                    fi
                fi

                if [[ ! -e "$path" && -n "$backup" && -f "$backup" ]]; then
                    mv "$backup" "$path"
                fi
                ;;

            file)
                if [[ -f "$path" ]]; then
                    if [[ "$(hash_file "$path")" == "$hash" ]]; then
                        rm -f "$path"
                    else
                        echo "Warning: modified installed file kept: $path" >&2
                        continue
                    fi
                fi

                if [[ ! -e "$path" && -n "$backup" && -f "$backup" ]]; then
                    mv "$backup" "$path"
                fi
                ;;

            reg)
                if command -v wine >/dev/null 2>&1; then
                    if [[ "$backup" == "MISSING" ]]; then
                        wine reg delete "$path" /v "$hash" /f >/dev/null
                    else
                        wine reg add "$path" /v "$hash" /t REG_DWORD /d "$backup" /f >/dev/null
                    fi
                fi
                ;;
        esac
    done < "$manifest"

    rm -f "$manifest"

    if [[ "$target" == "$script_dir" && "$script_name" == "linux_uninstall.sh" ]]; then
        mv "$script_path" "$script_dir/linux_install.sh"
    fi

    echo "dlssg-to-fsr3 removed from $target"
    exit 0
}

if [[ "$script_name" == "linux_uninstall.sh" || ( -f "$manifest" && -z "$mode" ) ]]; then
    uninstall=1
fi

if (( uninstall )); then
    uninstall_files
fi

valid_modes=" version winhttp dbghelp asi red4ext nvngx optiscaler "

if [[ -z "$mode" ]]; then
    echo "[1] version.dll"
    echo "[2] winhttp.dll"
    echo "[3] dbghelp.dll"
    echo "[4] ASI plugin"
    echo "[5] RED4ext plugin"
    echo "[6] nvngx.dll"
    echo "[7] OptiScaler"
    echo "[8] Uninstall"
    echo

    read -r -p "Enter 1-8 (or press Enter for default): " selection
    selection="${selection:-1}"

    case "$selection" in
        1) mode="version" ;;
        2) mode="winhttp" ;;
        3) mode="dbghelp" ;;
        4) mode="asi" ;;
        5) mode="red4ext" ;;
        6) mode="nvngx" ;;
        7) mode="optiscaler" ;;
        8) uninstall_files ;;
        *)
            echo "Invalid selection: $selection" >&2
            exit 1
            ;;
    esac
fi

[[ "$valid_modes" == *" $mode "* ]] || {
    echo "Invalid mode: $mode" >&2
    exit 1
}

case "$mode" in
    version|winhttp|dbghelp) dll_name="$mode.dll" ;;
    asi) dll_name="dlssg_to_fsr3.asi" ;;
    red4ext) dll_name="dlssg_to_fsr3.dll" ;;
    nvngx) dll_name="nvngx.dll" ;;
    optiscaler) dll_name="dlssg_to_fsr3_amd_is_better.dll" ;;
esac

[[ ! -f "$manifest" ]] || {
    echo "Existing installation found. Run linux_uninstall.sh first." >&2
    exit 1
}

: > "$manifest.tmp"
echo "mode|$mode||" >> "$manifest.tmp"

install_moved_file() {
    local source="$1"
    local destination="$2"
    local backup=""

    [[ -f "$source" ]] || {
        echo "Package file missing: $source" >&2
        exit 1
    }

    if [[ "$source" != "$destination" && -f "$destination" ]]; then
        if (( ! force )); then
            read -r -p "$destination exists. Back up and replace? [y/N] " answer
            [[ "$answer" =~ ^[Yy]([Ee][Ss])?$ ]] || exit 1
        fi

        backup="$destination.dlssg_to_fsr3.bak"

        [[ ! -e "$backup" ]] || {
            echo "Backup already exists: $backup" >&2
            exit 1
        }

        mv "$destination" "$backup"
    fi

    if [[ "$source" != "$destination" ]]; then
        mv "$source" "$destination"
    fi

    echo "move|$destination|$(hash_file "$destination")|$backup|$source" >> "$manifest.tmp"
}

install_file() {
    local source="$1"
    local destination="$2"
    local backup=""

    [[ -f "$source" ]] || {
        echo "Package file missing: $source" >&2
        exit 1
    }

    if [[ -e "$destination" && "$source" -ef "$destination" ]]; then
        return
    fi

    if [[ -f "$destination" ]]; then
        if (( ! force )); then
            read -r -p "$destination exists. Back up and replace? [y/N] " answer
            [[ "$answer" =~ ^[Yy]([Ee][Ss])?$ ]] || exit 1
        fi

        backup="$destination.dlssg_to_fsr3.bak"

        [[ ! -e "$backup" ]] || {
            echo "Backup already exists: $backup" >&2
            exit 1
        }

        mv "$destination" "$backup"
    fi

    cp "$source" "$destination"
    echo "file|$destination|$(hash_file "$destination")|$backup" >> "$manifest.tmp"
}

install_moved_file "$script_dir/dlssg_to_fsr3.dll" "$target/$dll_name"
install_file "$script_dir/amd_fidelityfx_dx12.dll" "$target/amd_fidelityfx_dx12.dll"

if [[ "$mode" == "nvngx" && $signature_override -eq 0 ]]; then
    read -r -p "Enable Nvidia signature override in active Wine prefix? [y/N] " answer
    [[ "$answer" =~ ^[Yy]([Ee][Ss])?$ ]] && signature_override=1
fi

if [[ "$mode" == "nvngx" && $signature_override -eq 1 ]]; then
    command -v wine >/dev/null 2>&1 || {
        echo "wine is required to configure signature override." >&2
        exit 1
    }

    value_name="{41FCC608-8496-4DEF-B43E-7D9BD675A6FF}"

    for key in \
        'HKLM\SOFTWARE\NVIDIA Corporation\Global' \
        'HKLM\SYSTEM\ControlSet001\Services\nvlddmkm'
    do
        old_value="$(
            wine reg query "$key" /v "$value_name" 2>/dev/null |
                awk 'NF { value=$NF } END { print value }'
        )"

        old_value="${old_value:-MISSING}"
        echo "reg|$key|$value_name|$old_value" >> "$manifest.tmp"
        wine reg add "$key" /v "$value_name" /t REG_DWORD /d 1 /f >/dev/null
    done
fi

mv "$manifest.tmp" "$manifest"

if [[ "$target" == "$script_dir" && "$script_name" == "linux_install.sh" ]]; then
    mv "$script_path" "$script_dir/linux_uninstall.sh"
fi

echo "Installed $mode mode to $target"