# Pinned by digest so a build uses exactly the image that was reviewed; Dependabot
# (.github/dependabot.yml) proposes the new digest when the image moves.
FROM gcr.io/oss-fuzz-base/base-builder@sha256:ed4aefea5aa72bb8c133576dd13a1417e0fc6eeb27cc0fc7f9880e7e13293b07
RUN apt-get update && apt-get install -y --no-install-recommends liblz4-dev zip
COPY . $SRC/abm-io
WORKDIR $SRC/abm-io
COPY .clusterfuzzlite/build.sh $SRC/
