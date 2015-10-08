#!/bin/bash


function doMove {
            chunkserverIP=$1
            #echo $chunkKillScriptName
            #echo $chunkRunScriptName
            ssh ubuntu@$chunkserverIP "$chunkKillScriptName"
}

function doSCP {
            chunkserverIP=$1
            #fName="/home/ubuntu/doCompile.sh"
            fName=$chunkKillScriptName
            scp $fName ubuntu@$chunkserverIP:/home/ubuntu/
}

targetDir=$1

#chunkservers="192.168.1.225 192.168.1.226 192.168.1.227 192.168.1.228 192.168.1.229 192.168.1.230 192.168.1.231 192.168.1.233 192.168.1.235 192.168.1.236"
#chunkservers="192.168.1.155 192.168.1.145 192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149 192.168.1.15 192.168.1.150 192.168.1.52 192.168.1.56"
chunkservers=$(cat "/home/ubuntu/all_server_list.txt")

chunkKillScriptName=""
#mode=$2
#if [ "$mode" == "orig" ]
#then
#	chunkKillScriptName="/home/ubuntu/moveBuild_orig.sh $targetDir"
#elif [ "$mode" == "repair" ]
#then
	chunkKillScriptName="/home/ubuntu/moveBuild.sh $targetDir"
#fi

$chunkKillScriptName

for s in $chunkservers
do
	echo "Processing $s"
#        scp $chunkKillScriptName ubuntu@$s:/home/ubuntu/
        doSCP $s
        doMove $s
done

