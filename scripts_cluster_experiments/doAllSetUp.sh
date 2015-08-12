#!/bin/bash

./removeFromAll.sh /home/ubuntu/codes/qfs-repair/build8MB
./removeFromAll.sh /home/ubuntu/codes/qfs-original/build8MB

./removeFromAll.sh /home/ubuntu/codes/qfs-repair/build16MB
./removeFromAll.sh /home/ubuntu/codes/qfs-original/build16MB


./removeFromAll.sh /home/ubuntu/codes/qfs-repair/build32MB
./removeFromAll.sh /home/ubuntu/codes/qfs-original/build32MB


./removeFromAll.sh /home/ubuntu/codes/qfs-repair/build64MB
./removeFromAll.sh /home/ubuntu/codes/qfs-original/build64MB


./copyChunkSizeFiles.sh 8MB
./getCodeAndCompile.sh
./moveBuildDirectory.sh 8MB

./copyChunkSizeFiles.sh 16MB
./getCodeAndCompile.sh
./moveBuildDirectory.sh 16MB

./copyChunkSizeFiles.sh 32MB
./getCodeAndCompile.sh
./moveBuildDirectory.sh 32MB


./copyChunkSizeFiles.sh 64MB
./getCodeAndCompile.sh
./moveBuildDirectory.sh 64MB


wait
echo "Done all compiling"

