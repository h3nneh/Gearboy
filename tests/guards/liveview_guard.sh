#!/usr/bin/env bash
#
# Gearboy live-view conformance guard.
#
# Enforces the machine-checkable invariants of docs/sdd/live-view/spec.md:
#   D7 - no throw/try/catch in platforms/shared/desktop/liveview/
#   D5 - the embedded viewer page references no external http(s) resources
#   D1 - no new entries below platforms/shared/dependencies/ (no new dependencies)
#
# Exits 0 when every invariant holds, non-zero with a naming message otherwise.

set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"

liveview_dir="$repo_root/platforms/shared/desktop/liveview"
deps_dir="$repo_root/platforms/shared/dependencies"

# The GPL boilerplate carries the license URL; it is not an external resource
# of the viewer page.
license_url_pattern='www\.gnu\.org/licenses'

failed=0

fail()
{
    printf 'liveview_guard: FAIL [%s] %s\n' "$1" "$2" >&2
    failed=1
}

note()
{
    printf 'liveview_guard: note [%s] %s\n' "$1" "$2"
}

liveview_files()
{
    [ -d "$liveview_dir" ] || return 0
    find "$liveview_dir" -type f | LC_ALL=C sort
}

# D7 - no exception keywords in the live-view module.
check_no_exceptions()
{
    if [ ! -d "$liveview_dir" ]; then
        note "D7" "$liveview_dir does not exist yet, nothing to check"
        return
    fi

    local file hits
    while IFS= read -r file; do
        [ -n "$file" ] || continue
        hits="$(grep -InE '\<(throw|try|catch)\>' "$file")" || continue
        while IFS= read -r hit; do
            [ -n "$hit" ] || continue
            fail "D7" "exception keyword in ${file#$repo_root/}:$hit"
        done <<< "$hits"
    done <<< "$(liveview_files)"
}

# D5 - the embedded viewer page loads no external resources.
viewer_page_files()
{
    local file
    while IFS= read -r file; do
        [ -n "$file" ] || continue
        case "$(basename "$file" | tr '[:upper:]' '[:lower:]')" in
            *viewer*)
                echo "$file"
                continue
                ;;
        esac
        if grep -Iqi '<html' "$file"; then
            echo "$file"
        fi
    done <<< "$(liveview_files)"
}

check_no_external_resources()
{
    if [ ! -d "$liveview_dir" ]; then
        note "D5" "$liveview_dir does not exist yet, no viewer page to check"
        return
    fi

    local pages file hits
    pages="$(viewer_page_files)"
    if [ -z "$pages" ]; then
        note "D5" "no viewer page found below ${liveview_dir#$repo_root/}"
        return
    fi

    while IFS= read -r file; do
        [ -n "$file" ] || continue
        hits="$(grep -InE 'https?://' "$file" | grep -v "$license_url_pattern")" || continue
        while IFS= read -r hit; do
            [ -n "$hit" ] || continue
            fail "D5" "external resource in viewer page ${file#$repo_root/}:$hit"
        done <<< "$hits"
    done <<< "$pages"
}

# D1 - no new dependencies below platforms/shared/dependencies/.
check_no_new_dependencies()
{
    if [ ! -d "$deps_dir" ]; then
        fail "D1" "${deps_dir#$repo_root/} is missing"
        return
    fi

    if ! git -C "$repo_root" rev-parse --git-dir > /dev/null 2>&1; then
        fail "D1" "cannot verify dependencies: $repo_root is not a git repository"
        return
    fi

    local new_entries entry
    new_entries="$(git -C "$repo_root" ls-files --others --exclude-standard \
        -- platforms/shared/dependencies)"
    if [ -z "$new_entries" ]; then
        return
    fi

    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        fail "D1" "new dependency entry not in the committed tree: $entry"
    done <<< "$new_entries"
}

check_no_exceptions
check_no_external_resources
check_no_new_dependencies

if [ "$failed" -ne 0 ]; then
    printf 'liveview_guard: %s\n' "conformance guard failed" >&2
    exit 1
fi

printf 'liveview_guard: %s\n' "all invariants hold (D1, D5, D7)"
exit 0
