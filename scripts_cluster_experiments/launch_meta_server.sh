#!/bin/bash

#rm -rf /home/ubuntu/qfsbase.old/
#mv /home/ubuntu/qfsbase /home/ubuntu/qfsbase.old
rm -rf /home/ubuntu/qfsbase/

mkdir -p /home/ubuntu/qfsbase/meta/conf/
mkdir -p /home/ubuntu/qfsbase/meta/checkpoints/
mkdir -p /home/ubuntu/qfsbase/meta/logs/

buildDir=$1
exeName="/home/ubuntu/codes/qfs-original/$buildDir/debug/bin/metaserver"


$exeName '/home/ubuntu/MetaServer.prp' '/home/ubuntu/qfsbase/meta/MetaServer.log' > '/home/ubuntu/qfsbase/meta/MetaServer.out' 2>&1 &
#'/home/ubuntu/codes/qfs-original/build/debug/bin/metaserver' -c '/home/ubuntu/MetaServer.prp'  > '/home/ubuntu/qfsbase/meta/MetaServer.out' 2>&1 &
sleep 2
echo "MetaServer started with pid:"
cat /home/ubuntu/qfsbase/meta/metaserver.pid

echo "With properties:"
cat /home/ubuntu/MetaServer.prp
