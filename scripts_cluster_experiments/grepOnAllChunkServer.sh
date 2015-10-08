#!/bin/bash


function doGrep {
            chunkserverIP=$1
            grepStr=$2
            #echo $chunkKillScriptName
            #echo $chunkRunScriptName
            #ssh ubuntu@$chunkserverIP "grep \"$grepStr\" /home/ubuntu/qfsbase/*"
            searchStr="grep \"$grepStr\" /home/ubuntu/qfsbase/ChunkServer.log"
            echo $searchStr
            ssh ubuntu@$chunkserverIP "$searchStr"
}

#chunkservers="192.168.1.225 192.168.1.226 192.168.1.227 192.168.1.228 192.168.1.229 192.168.1.230 192.168.1.231 192.168.1.233 192.168.1.235 192.168.1.236"
#chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33"

chunkservers=$(cat "/home/ubuntu/chunk_server_list.txt")

theGrepStr=$1
echo "The string to be grepped on all servers is = $theGrepStr" 

for s in $chunkservers
do
	echo "On: $s"
#        scp $chunkKillScriptName ubuntu@$s:/home/ubuntu/
        doGrep $s "$theGrepStr"
done

