#!/bin/bash

#./kill_chunk_server.sh

mkdir -p /home/ubuntu/qfsbase/chunkdir11

buildDir=$1
exeName="/home/ubuntu/codes/qfs-repair/$buildDir/debug/bin/chunkserver"

$exeName '/home/ubuntu/ChunkServer.prp' '/home/ubuntu/qfsbase/ChunkServer.log' > '/home/ubuntu/qfsbase/ChunkServer.out' 2>&1 &
echo "Launched repair chunkserver with pid:"
cat /home/ubuntu/qfsbase/chunkserver.pid
echo "With properties:"
cat /home/ubuntu/ChunkServer.prp

#/home/ubuntu/takePeriodicNetStat.sh &

#sudo kill -9 `ps -ef | grep tcpflow | grep -v grep | awk '{print $2}'`

#sudo rm -rf tcpflowDump
#mkdir -p tcpflowDump
#cd tcpflowDump
#sudo tcpflow -i any -J  &

