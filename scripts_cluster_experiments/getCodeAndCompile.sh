#!/bin/bash


function doCompile {
            chunkserverIP=$1
            scriptName=$2
            #echo $chunkKillScriptName
            #echo $chunkRunScriptName
            ssh ubuntu@$chunkserverIP "$scriptName"
}

function doSCP {
            chunkserverIP=$1
            #fName="/home/ubuntu/doCompile.sh"
            fName=$chunkKillScriptName
            scp $fName ubuntu@$chunkserverIP:/home/ubuntu/
}

#chunkservers="192.168.1.225 192.168.1.226 192.168.1.227 192.168.1.228 192.168.1.229 192.168.1.230 192.168.1.231 192.168.1.233 192.168.1.235 192.168.1.236"
chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33 192.168.0.34"

chunkKillScriptName="/home/ubuntu/doCompile.sh"
chunkKillScriptNameOrig="/home/ubuntu/doCompile_orig.sh"

$chunkKillScriptName &
$chunkKillScriptNameOrig &

for s in $chunkservers
do
	echo "Processing $s"
#        scp $chunkKillScriptName ubuntu@$s:/home/ubuntu/
        doCompile $s $chunkKillScriptName &
        #doCompile $s $chunkKillScriptNameOrig &
done
wait
echo "Done all compiling"

