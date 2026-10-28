FROM gcr.io/oss-fuzz-base/base-builder
RUN apt-get update && apt-get install -y --no-install-recommends liblz4-dev zip
COPY . $SRC/abm-io
WORKDIR $SRC/abm-io
COPY .clusterfuzzlite/build.sh $SRC/
