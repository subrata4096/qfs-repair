#!/bin/bash


function doRm {
            chunkserverIP=$1
            fileStr=$2
            #echo $chunkKillScriptName
            #echo $chunkRunScriptName
            #ssh ubuntu@$chunkserverIP "grep \"$grepStr\" /home/ubuntu/qfsbase/*"
            rmStr="rm -rf \"$fileStr\""
            echo $rmStr
            ssh ubuntu@$chunkserverIP "$rmStr"
}

#chunkservers="192.168.1.225 192.168.1.226 192.168.1.227 192.168.1.228 192.168.1.229 192.168.1.230 192.168.1.231 192.168.1.233 192.168.1.235 192.168.1.236"
#chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33 192.168.0.34"
#chunkservers="192.168.1.155 192.168.1.145 192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149 192.168.1.15 192.168.1.150 192.168.1.52 192.168.1.56"

chunkservers=$(cat "/home/ubuntu/all_server_list.txt")

theFileToRemoveStr=$1
echo "Removing $theFileToRemoveStr on ALL the servers..." 
rm -rf "$theFileToRemoveStr"

for s in $chunkservers
do
	echo "Removing On: $s"
#        scp $chunkKillScriptName ubuntu@$s:/home/ubuntu/
        doRm $s "$theFileToRemoveStr"
done

