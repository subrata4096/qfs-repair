#!/bin/bash

function doSCP {
            chunkserverIP=$1
            fName=$2
            scp $fName ubuntu@$chunkserverIP:/home/ubuntu/
}

#chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33 192.168.0.34"
#chunkservers="192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.148 192.168.1.149 192.168.1.15 192.168.1.150"
#chunkservers="192.168.1.155 192.168.1.145 192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149 192.168.1.15 192.168.1.150 192.168.1.15 192.168.1.56"

chunkservers=$(cat "/home/ubuntu/all_server_list.txt")

#listOfFilesToCopy="ChunkServer.prp MetaServer.prp sample_setup.cfg kill_chunk_server_repair.sh kill_chunk_server.sh kill_meta_server.sh launch_chunk_server_repair.sh launch_chunk_server.sh launch_meta_server.sh run_experiment.sh"
#listOfFilesToCopy="ChunkServer.prp"
listOfFilesToCopy="doCompile_orig.sh doCompile.sh"

#listOfFilesToCopy="full_experiment.sh full_experiment_hdd_crash.sh"

#listOfFilesToCopy="checkDirs.sh ChunkServer.prp copyFilesToAll.sh doCompile_orig.sh doCompile.sh full_experiment.sh getCodeAndCompile.sh grepOnAllChunkServer.sh kill_chunk_server_repair.sh kill_chunk_server.sh kill_meta_server.sh launch_chunk_server_repair.sh launch_chunk_server.sh launch_meta_server.sh MetaServer.prp moveBuildDirectory.sh moveBuild_orig.sh moveBuild.sh run_experiment.sh runTCp.sh sample_setup.cfg"

#listOfFilesToCopy="all_server_list.txt chunk_server_list.txt bandwidth_limiting.sh checkDirs.sh ChunkServer.prp MetaServer.prp copyChunkSizeFiles.sh copyFilesToAll.sh doAllSetUp.sh doCompile_orig.sh doCompile.sh full_experiment.sh getCodeAndCompile.sh grepOnAllChunkServer.sh kill_chunk_server_repair.sh kill_chunk_server.sh kill_meta_server.sh launch_chunk_server_repair.sh launch_chunk_server.sh launch_meta_server.sh moveBuildDirectory.sh moveBuild_orig.sh moveBuild.sh removeFromAll.sh run_experiment.sh runTCp.sh setup_build.sh takePeriodicNetStat.sh"
 
for s in $chunkservers
do
	echo "Processing $s"
	for f in $listOfFilesToCopy
	do 
		echo "Copying.. $f"
        	doSCP $s $f
        done
done




