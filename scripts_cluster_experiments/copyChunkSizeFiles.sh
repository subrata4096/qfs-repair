#!/bin/bash

function doSCP {
            chunkserverIP=$1
            fName1=$2
            fName2=$3
            scp $fName1 ubuntu@$chunkserverIP:/home/ubuntu/codes/qfs-repair/examples/cc/qfssample_main.cc
            scp $fName2 ubuntu@$chunkserverIP:/home/ubuntu/codes/qfs-repair/src/cc/common/kfstypes.h
            
	    scp $fName1 ubuntu@$chunkserverIP:/home/ubuntu/codes/qfs-original/examples/cc/qfssample_main.cc
            scp $fName2 ubuntu@$chunkserverIP:/home/ubuntu/codes/qfs-original/src/cc/common/kfstypes.h

}

chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33 192.168.0.34"

fname1="qfssample_main.cc"
fname2="kfstypes.h"

chunkSize=$1
echo "You have set a CHUNK SIZE = $chunkSize"

f1="qfssample_main.cc_$chunkSize"
f2="kfstypes.h_$chunkSize"

cp $f1 /home/ubuntu/codes/qfs-repair/examples/cc/qfssample_main.cc
cp $f2 /home/ubuntu/codes/qfs-repair/src/cc/common/kfstypes.h

cp $f1 /home/ubuntu/codes/qfs-original/examples/cc/qfssample_main.cc
cp $f2 /home/ubuntu/codes/qfs-original/src/cc/common/kfstypes.h

 
for s in $chunkservers
do

	echo "Copying.. $f1, $f2"
       	doSCP $s $f1 $f2
done




