#!/bin/sh
# Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
# Apache License 2.0, see LICENSE and NOTICE.
#
# Runs tsMuxeR as USER_ID:GROUP_ID (1000:1000 if not given), so that what it writes in /storage belongs to that user
# on the host. A container started with --user is already that user: the program is run as it is. Its exit code is
# tsMuxeR's (docs/USAGE.md: 0 done, 1 the command line or meta file, 2 a source, 3 the output, 4 stopped, ...).
# tini starts this script, so tsMuxeR is not the first process of the container: Ctrl+C reaches it.
set -e
if [ "$(id -u)" = "0" ]; then
    # each number on its own: USER_ID= with GROUP_ID=1000 must not pass as "1000"
    for v in "$USER_ID" "$GROUP_ID"; do
        case "$v" in
        "" | *[!0-9]*)
            echo "USER_ID and GROUP_ID are numbers, such as USER_ID=1000 GROUP_ID=1000." >&2
            exit 1
            ;;
        esac
    done
    exec setpriv --reuid="$USER_ID" --regid="$GROUP_ID" --clear-groups --no-new-privs -- /usr/bin/tsMuxeR "$@"
fi
exec /usr/bin/tsMuxeR "$@"
