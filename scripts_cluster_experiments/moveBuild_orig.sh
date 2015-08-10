#!/bin/bash

targetChunkSize=$1
targetDir="build$targetChunkSize"

cd /home/ubuntu/codes/qfs-repair/
cp -r build $targetDir

cd /home/ubuntu/codes/qfs-original/
cp -r build $targetDir

