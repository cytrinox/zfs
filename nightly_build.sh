#!/bin/bash

COMMIT_VERSION=$(git describe | sed 's,\(.*\)-\(.*\)-\(.*\)-\(.*\),\2,')
COMMIT_PATCHLEVEL=$(git describe | sed 's,\(.*\)-\(.*\)-\(.*\)-\(.*\),git.\3+\4,')

# Remove changelog if exists
rm -f debian/changelog

# Create empty changelog and set version number
dch --create --distribution unstable --package zfs-linux -v "$COMMIT_VERSION-$COMMIT_PATCHLEVEL-1" "Nightly Build"

# Generate changelog, include last 16 commit messages
gbp dch  --since=HEAD~16 -R --spawn-editor=never --ignore-branch

# Build DKMS deb package
dpkg-buildpackage -rfakeroot -b -us -uc

