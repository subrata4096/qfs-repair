#!/bin/bash

targetChunkSize=$1
targetDir="build$targetChunkSize"

com="mv build $targetDir"
echo "$com"
cd /home/ubuntu/codes/qfs-repair/
$com

cd /home/ubuntu/codes/qfs-original/
$com

