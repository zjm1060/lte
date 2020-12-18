#!/bin/sh

ProjName=$1

cross_prefix=arm-linux-gnueabihf-

Path=$(cd "$(dirname "$0")"; pwd)

echo $Path

${cross_prefix}objcopy --only-keep-debug ${ProjName} ${ProjName}.sym 
${cross_prefix}strip --strip-all ${ProjName} 
${cross_prefix}objcopy --add-gnu-debuglink=${ProjName}.sym  ${ProjName}

DIST_DIR="$Path/deploy"

cp -a $ProjName $DIST_DIR/
