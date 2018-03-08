#!/bin/sh
set -e

cd /usr/src/linux
git diff 3314a93f52e2fbb6dd3f9279e6ac0efb6cbddf91 HEAD^ > ~/patch
echo "Patch created in ~/patch"
