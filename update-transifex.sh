#!/bin/sh

test -f .tx/config || exit 1

echo Pulling translations from Transifex
tx --root `dirname $0` pull --all --force --skip

echo Pushing strings to Transifex
tx push --source
