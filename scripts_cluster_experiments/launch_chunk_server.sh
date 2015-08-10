#!/bin/bash

#./kill_chunk_server.sh

buildDir=$1
mkdir -p /home/ubuntu/qfsbase/chunkdir11
exeName="/home/ubuntu/codes/qfs-original/$buildDir/debug/bin/chunkserver"

$exeName '/home/ubuntu/ChunkServer.prp' '/home/ubuntu/qfsbase/ChunkServer.log' > '/home/ubuntu/qfsbase/ChunkServer.out' 2>&1 &
echo "Launched chunkserver with pid:"
cat /home/ubuntu/qfsbase/chunkserver.pid
echo "With properties:"
cat /home/ubuntu/ChunkServer.prp

#/home/ubuntu/takePeriodicNetStat.sh &
