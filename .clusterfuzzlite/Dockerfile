# Pinned by digest so a build uses exactly the image that was reviewed; Dependabot
# (.github/dependabot.yml) proposes the new digest when the image moves.
FROM gcr.io/oss-fuzz-base/base-builder@sha256:1f9cf74bc3d7029b528494542f9d9851b256dceed38d3f420989434dc5ae0760
RUN apt-get update && apt-get install -y --no-install-recommends liblz4-dev zip
COPY . $SRC/abm-io
WORKDIR $SRC/abm-io
COPY .clusterfuzzlite/build.sh $SRC/
