#!/usr/bin/env bash

# do NOT set -x or it will log the secret tokens!
set -e

if [[ $BUILD_TYPE == "default" && -z $DRAFT ]]; then
    # Tell travis to deploy all files in core/dist
    mkdir -p core/dist
    export LIBZLINK_DEPLOYMENT=core/dist/*
    # Move archives to core/dist
    mv *.tar.gz core/dist
    mv *.zip core/dist
    # Generate hash sums
    cd core/dist
    md5sum *.zip *.tar.gz > MD5SUMS
    sha1sum *.zip *.tar.gz > SHA1SUMS
    cd -
else
    export LIBZLINK_DEPLOYMENT=""
fi
