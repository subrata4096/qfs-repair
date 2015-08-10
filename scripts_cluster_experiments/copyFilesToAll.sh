#!/bin/bash

function doSCP {
            chunkserverIP=$1
            fName=$2
            scp $fName ubuntu@$chunkserverIP:/home/ubuntu/
}

chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33 192.168.0.34"

#listOfFilesToCopy="ChunkServer.prp MetaServer.prp sample_setup.cfg kill_chunk_server_repair.sh kill_chunk_server.sh kill_meta_server.sh launch_chunk_server_repair.sh launch_chunk_server.sh launch_meta_server.sh run_experiment.sh"
listOfFilesToCopy="ChunkServer.prp"
#listOfFilesToCopy="launch_chunk_server_repair.sh takePeriodicNetStat.sh"

#listOfFilesToCopy="checkDirs.sh ChunkServer.prp copyFilesToAll.sh doCompile_orig.sh doCompile.sh full_experiment.sh getCodeAndCompile.sh grepOnAllChunkServer.sh kill_chunk_server_repair.sh kill_chunk_server.sh kill_meta_server.sh launch_chunk_server_repair.sh launch_chunk_server.sh launch_meta_server.sh MetaServer.prp moveBuildDirectory.sh moveBuild_orig.sh moveBuild.sh run_experiment.sh runTCp.sh sample_setup.cfg"

 
for s in $chunkservers
do
	echo "Processing $s"
	for f in $listOfFilesToCopy
	do 
		echo "Copying.. $f"
        	doSCP $s $f
        done
done




