# Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
# Apache License 2.0, see LICENSE and NOTICE.
#
# tsMuxeR, the command line program, in a container (docs/DOCKER.md).
#
#   docker build -f packaging/docker/Dockerfile -t tsmuxer .
#   docker run --rm -v "$PWD:/storage" tsmuxer film.mkv
#   docker run --rm -v "$PWD:/storage" tsmuxer film.meta film.ts
#
# The folder mounted at /storage is the working folder: paths in a meta file are relative to it. /config is for
# files of the user's own that jobs use, such as an ffmpeg for the audio conversion (--ffmpeg=/config/ffmpeg).
# ffmpeg is not part of this image.
#
# TSMUXER_GIT_REV names the commit where the build has no .git (the source is copied without it).

FROM ubuntu:24.04 AS build
ARG TSMUXER_GIT_REV=
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config libfreetype-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTSMUXER_GUI=OFF \
        -DTSMUXER_GIT_REV="${TSMUXER_GIT_REV}" \
    && cmake --build /build --target tsmuxer \
    && strip /build/tsMuxer/tsmuxer

FROM ubuntu:24.04
# libass draws ASS and SSA subtitles (loaded when a job needs it); the fonts are for subtitles drawn as pictures;
# setpriv (util-linux) runs the program as USER_ID and GROUP_ID; tini is the first process of the container, so that
# Ctrl+C and docker stop reach the program (the first process of a container gets no signal it has no handler for)
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libfreetype6 zlib1g libass9 fonts-liberation fonts-dejavu-core util-linux tini \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /storage /config /usr/share/doc/tsmuxer \
    && dpkg-query -W -f='${Package} ${Version}\n' > /usr/share/doc/tsmuxer/PACKAGES.txt
COPY --from=build /build/tsMuxer/tsmuxer /usr/bin/tsMuxeR
COPY packaging/docker/entrypoint.sh /usr/local/bin/tsmuxer-entrypoint
COPY LICENSE NOTICE /usr/share/doc/tsmuxer/
COPY packaging/notices/Docker.txt /usr/share/doc/tsmuxer/THIRD-PARTY-NOTICES.txt
COPY licenses /usr/share/doc/tsmuxer/licenses
# a copy checked out on Windows may have CRLF line ends, and the script would not start
RUN sed -i 's/\r$//' /usr/local/bin/tsmuxer-entrypoint && chmod 755 /usr/local/bin/tsmuxer-entrypoint
LABEL org.opencontainers.image.title="tsMuxeR" \
      org.opencontainers.image.description="tsMuxeR command line: mux and demux transport streams, Blu-ray and Matroska" \
      org.opencontainers.image.source="https://github.com/teaching-droid/tsMuxer" \
      org.opencontainers.image.licenses="Apache-2.0"
# The program runs as USER_ID:GROUP_ID (1000:1000 if not given), so the files it writes in /storage belong to that
# user on the host; started with --user, it runs as that user instead. The font cache goes to /tmp, not into the
# user's /config folder.
ENV USER_ID=1000 GROUP_ID=1000 HOME=/config XDG_CACHE_HOME=/tmp/cache
WORKDIR /storage
VOLUME ["/storage", "/config"]
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/tsmuxer-entrypoint"]
