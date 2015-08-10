#!/bin/bash


chunkSizeList="32MB 64MB"
numExpId="1 2 3 4 5"
failureCount="1 2 4 8 10"

modeList="repair orig"

#metaServerOrigName="/home/ubuntu/qfsbase/meta/MetaServer.log"
#metaServerOrigName1="/home/ubuntu/qfsbase/meta/MetaServer.log.1"
#metaServerOrigName2="/home/ubuntu/qfsbase/meta/MetaServer.log.2"

experimentLogBaseDir="/home/ubuntu/experimentLogBaseDir"

metaServerIP="135.197.240.179"


for chunkSize in $chunkSizeList
do
   for fCount in $failureCount
   do
        numFilesToWrite=$((fCount + 2))
      
        for e in $numExpId
        do
             for mode in $modeList
             do
 
                 
                 ssh ubuntu@$metaServerIP "reboot"
                 sleep 10

                 echo "EXPERIMENT ID: $e with chunkSize = $chunkSize   $mode with Failure Count = $fCount with files written $numFilesToWrite"

                 targetLogDir="$experimentLogBaseDir/$chunkSize/failures_$fCount/$mode/exp_$e/"
             
	         theCommand="/home/ubuntu/run_experiment.sh $mode1 $numFilesToWrite $fCount $targetLogDir"
                 echo $theCommand
                 #$theCommand
            done     
        done
   done
done
