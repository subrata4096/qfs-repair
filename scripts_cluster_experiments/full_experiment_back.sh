#!/bin/bash


chunkSize="32MB"
numExpId="1 2 3"
failureCount="1 2 4 8 10"

mode1="repair"
mode2="orig"

metaServerOrigName="/home/ubuntu/qfsbase/meta/MetaServer.log"
metaServerOrigName1="/home/ubuntu/qfsbase/meta/MetaServer.log.1"
metaServerOrigName2="/home/ubuntu/qfsbase/meta/MetaServer.log.2"

experimentLogBaseDir="/home/ubuntu/experimentLogBaseDir"
experimentForChunkDir="$experimentLogBaseDir/$chunkSize"


sleep 3000

for fCount in $failureCount
do
        numFilesToWrite=$((fCount + 2))
        metaServerRename=
        echo " Failure Count = $fCount with files written $numFilesToWrite"

        fCountDir1="$experimentForChunkDir/failures_$fCount/$mode1"
        fCountDir2="$experimentForChunkDir/failures_$fCount/$mode2"
      
        mkdir -p $fCountDir1
        mkdir -p $fCountDir2

        for e in $numExpId
        do
             echo "EXPERIMENT ID: $e with chunkSize = $chunkSize   $mode1"
             metaServerRename="$fCountDir1/MetaServer_exp_$e.log"
             metaServerRename1="$fCountDir1/MetaServer_exp_$e.log.1"
             metaServerRename2="$fCountDir1/MetaServer_exp_$e.log.2"
             
	     theCommand="/home/ubuntu/run_experiment.sh $mode1 $numFilesToWrite $fCount"
             echo $theCommand
             $theCommand
             
             sleepTime=50
             echo "Give time to repair: $sleepTime sec"
             sleep $sleepTime

             mv $metaServerOrigName $metaServerRename
             mv $metaServerOrigName1 $metaServerRename1
             mv $metaServerOrigName2 $metaServerRename2

             
             echo "EXPERIMENT ID: $e with chunkSize = $chunkSize   $mode2"
             metaServerRename="$fCountDir2/MetaServer_exp_$e.log"
             metaServerRename1="$fCountDir2/MetaServer_exp_$e.log.1"
             metaServerRename2="$fCountDir2/MetaServer_exp_$e.log.2"
             
	     theCommand="/home/ubuntu/run_experiment.sh $mode2 $numFilesToWrite $fCount"
             echo $theCommand
             $theCommand
             
             sleepTime=50
             echo "Give time to repair: $sleepTime sec"
             sleep $sleepTime

             mv $metaServerOrigName $metaServerRename
             mv $metaServerOrigName1 $metaServerRename1
             mv $metaServerOrigName2 $metaServerRename2
        done
done
