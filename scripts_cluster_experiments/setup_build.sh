#!/bin/bash

#chunkservers="192.168.1.155 192.168.1.145 192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149 192.168.1.15 192.168.1.150 192.168.1.52 192.168.1.56"
chunkservers=$(cat "/home/ubuntu/all_server_list.txt")

#listOfFilesToCopy="ChunkServer.prp bandwidth_limiting.sh"
#listOfFilesToCopy_orig="ChunkServer.prp bandwidth_limiting.sh"

function doSCPOrig {
            chunkserverIP=$1
            scp kfstypes.h ubuntu@$chunkserverIP:/home/ubuntu/codes/qfs-original/src/cc/common/kfstypes.h
            scp qfssample_main.cc ubuntu@$chunkserverIP:/home/ubuntu/codes/qfs-original/examples/cc/qfssample_main.cc
}

function doSCPRepair {
            chunkserverIP=$1
            scp kfstypes.h ubuntu@$chunkserverIP:/home/ubuntu/codes/qfs-repair/src/cc/common/kfstypes.h
            scp qfssample_main.cc ubuntu@$chunkserverIP:/home/ubuntu/codes/qfs-repair/examples/cc/qfssample_main.cc
            scp LayoutManager.cc ubuntu@$chunkserverIP:/home/ubuntu/codes/qfs-repair/src/cc/meta/LayoutManager.cc
}

for s in $chunkservers
do
        echo "Processing $s"
        doSCPRepair $s
        doSCPOrig $s
done
