#!/bin/bash


chunkSizeList="64MB 32MB 16MB"
#chunkSizeList="8MB"
numExpId="1 2 3"
#numExpId="1"
#failureCount="1 2 4 8 10 12"
#failureCount="1 2 4 8 10 12"
#failureCount="1"

declare -A fileCount
declare -A ChunkFailCount

fileCount=( ["6_3"]="1700" ["12_4"]="1000")
ChunkFailCount=( ["6_3"]="120" ["12_4"]="120")


codingList="6_3 12_4"

modeList="repair orig"

#metaServerOrigName="/home/ubuntu/qfsbase/meta/MetaServer.log"
#metaServerOrigName1="/home/ubuntu/qfsbase/meta/MetaServer.log.1"
#metaServerOrigName2="/home/ubuntu/qfsbase/meta/MetaServer.log.2"

experimentLogBaseDir="/home/ubuntu/experimentLogBaseDir"

#metaServerIP="135.197.240.179"

for code in $codingList
do
  for e in $numExpId
  do
    for chunkSize in $chunkSizeList
      do
       #for fCount in $failureCount
       #do
         #numFilesToWrite=$((fCount + 2))
         numFilesToWrite=${fileCount[$code]}
	 fCount=${ChunkFailCount[$code]}
      
         for mode in $modeList
         do
 
                 buildDir="build$chunkSize""_$code"
                 #echo "Trying to reboot MetaServer VM: $metaServerIP"
 
                 #ssh ubuntu@$metaServerIP "sudo reboot"
                 #sleep 60

                 echo "EXPERIMENT ID: $e with chunkSize = $chunkSize  $code $mode with Failure Count = $fCount with files written $numFilesToWrite"

                 targetLogDir="$experimentLogBaseDir/$code/$chunkSize/failures_$fCount/$mode/exp_$e/"
             
	         theCommand="/home/ubuntu/run_experiment.sh $mode $numFilesToWrite $fCount $buildDir $targetLogDir"
                 echo $theCommand
                 $theCommand
         done     
       #done
    done
  done
done
