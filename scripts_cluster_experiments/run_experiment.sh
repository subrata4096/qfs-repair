#!/bin/bash


function doKill {
            chunkserverIP=$1
            #echo $chunkKillScriptName
            #echo $chunkRunScriptName
            ssh ubuntu@$chunkserverIP 'bash -s' < $chunkKillScriptName
}


function doMoveChunkServerLogs {
            chunkserverIP=$1
            #echo $chunkKillScriptName
            #echo $chunkRunScriptName
            targetDir=$2
            ssh ubuntu@$chunkserverIP "mkdir -p $targetDir"
            ssh ubuntu@$chunkserverIP "mv /home/ubuntu/qfsbase/ChunkServer.* $targetDir/"
}


function doRestart {
            chunkserverIP=$1
            #echo $chunkKillScriptName
            #echo $chunkRunScriptName
            ssh ubuntu@$chunkserverIP "sudo reboot"
}

function doSetup {
            chunkserverIP=$1
            #echo $chunkKillScriptName
            #echo $chunkRunScriptName
            #ssh ubuntu@$chunkserverIP 'bash -s' < $chunkKillScriptName
            ssh ubuntu@$chunkserverIP "$chunkRunScriptName $buildDir"
            #com='rm -rf ~/qfsbase'
            #ssh ubuntu@$chunkserverIP 'bash -s' < $com
            #$chunkKillScriptName
            #$chunkRunScriptName
            #exit
}


function doSCP {
            chunkserverIP=$1
            fName=$2
            scp $fName ubuntu@$chunkserverIP:/home/ubuntu/
}

function doTCPDump {

            chunkserverIP=$1
            ssh $chunkserverIP "sudo tcpdump -i eth0 -s 0 -U -w - not port 22" > /tmp/pipe

}

function countChunksOnChunkServer {

            chunkserverIP=$1
            
            numChunks=$(ssh -o StrictHostKeyChecking=no $chunkserverIP 'find /home/ubuntu/qfsbase/chunkdir11/ -name "*.*.*" | wc -l')

            echo $numChunks
}

chunkRunScriptName=""
#chunkRunScriptName="/home/ubuntu/launch_chunk_server.sh"
#metaRunScriptName="/home/ubuntu/launch_meta_server.sh"

chunkKillScriptName="/home/ubuntu/kill_chunk_server.sh"
metaKillScriptName="/home/ubuntu/kill_meta_server.sh"

mode=$1
numFiles=$2  # number of files to use for creating chunks
numChunksToLose=$3  # number of chunks to kill. We will check all the chunk servers and will kill the one that has this much number of chunks in it
buildDir=$4
if [ -z "$4" ]; then
        
        echo "Please provide a build dir. Example: build64MB, build32MB, or build. etc."
        echo "help : ./run_experiment.sh repair 15 12 build32MB " 
        exit
fi

targetDirForExp=""
if [ ! -z "$5" -a "$5" != " " ]; then
	targetDirForExp=$5
        echo "Creating target dir: $targetDirForExp"
        mkdir -p $targetDirForExp
fi

#metaServerIp=$3
metaServerIp="192.168.0.235"
#if [ "$metaServerIp" == ""]
#then
#     metaServerIp="192.168.0.235"
#fi

echo "Metaserver IP to be used for client is : $metaServerIp " 

qfscodedirname=""
qfsclientBinaryPath="/home/ubuntu/codes/"
if [ "$mode" == "orig" ]
then 
   	echo "Running Original QFS"
  	chunkRunScriptName="/home/ubuntu/launch_chunk_server.sh"
	#metaRunScriptName="/home/ubuntu/launch_meta_server.sh"

	#chunkKillScriptName="/home/ubuntu/kill_chunk_server.sh"
	#metaKillScriptName="/home/ubuntu/kill_meta_server.sh"
	
	qfscodedirname="qfs-original"
        qfsclientBinaryPath="/home/ubuntu/codes/qfs-original/$buildDir/debug/examples/cc/qfssample -s $metaServerIp -p 20000 -f $numFiles"

elif [ "$mode" == "repair" ]
then
	echo "Running Repair QFS"
	chunkRunScriptName="/home/ubuntu/launch_chunk_server_repair.sh"
	#metaRunScriptName="/home/ubuntu/launch_meta_server_repair.sh"

	#chunkKillScriptName="/home/ubuntu/kill_chunk_server_repair.sh"
	#metaKillScriptName="/home/ubuntu/kill_meta_server_repair.sh"
	#chunkKillScriptName="/home/ubuntu/kill_chunk_server.sh"
	#metaKillScriptName="/home/ubuntu/kill_meta_server.sh"
	
	qfscodedirname="qfs-repair"
        qfsclientBinaryPath="/home/ubuntu/codes/qfs-repair/$buildDir/debug/examples/cc/qfssample -s $metaServerIp -p 20000 -f $numFiles"
else
   exit 1
fi

qfsdir="/home/ubuntu/codes/"$qfscodedirname


#sudo service ntp stop
#sudo ntpdate -s time.nist.gov
#sudo service ntp start


#$metaKillScriptName
#$qfsdir/examples/sampleservers/sample_setup.py -a uninstall
#rm -rf ~/qfsbase

#$qfsdir/examples/sampleservers/sample_setup.py -a install
 
#$metaRunScriptName

#Cirrus5chunkservers="192.168.1.225 192.168.1.226 192.168.1.227 192.168.1.228 192.168.1.229 192.168.1.230 192.168.1.231 192.168.1.233 192.168.1.235 192.168.1.236"
chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33"

#listOfFilesToCopy="ChunkServer.prp MetaServer.prp sample_setup.cfg kill_chunk_server_repair.sh kill_chunk_server.sh kill_meta_server.sh launch_chunk_server_repair.sh launch_chunk_server.sh launch_meta_server.sh run_experiment.sh"
listOfFilesToCopy="ChunkServer.prp"

qfsClientIp=192.168.0.34
 
#for s in $chunkservers
#do
#	echo "Processing $s"
#	for f in $listOfFilesToCopy
#	do 
#		echo "Copying.. $f"
#        	doSCP $s $f
#        done
#done

for s in $chunkservers
do
	echo "Rebooting ..  $s"
#        scp $chunkKillScriptName ubuntu@$s:/home/ubuntu/
        doKill $s
        doRestart $s
done
sleep 50

./kill_meta_server.sh

metaReleaseDir="$qfsdir/$buildDir/debug/"

$qfsdir/examples/sampleservers/sample_setup.py -r $metaReleaseDir -c /home/ubuntu/sample_setup.cfg -a uninstall

sleep 1


#echo "***   Netstat BEFORE metaserver launch  **** "
#echo "-------------------------------------------- "
#netstat

$qfsdir/examples/sampleservers/sample_setup.py -r $metaReleaseDir -c /home/ubuntu/sample_setup.cfg -a install


#echo "***   Netstat AFTER metaserver launch  **** "
#echo "-------------------------------------------- "
#netstat

sleep 10


chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33"
#chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33"
#chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29"


for f in $chunkservers
do
        echo "Setting up $f"
#        scp $chunkRunScriptName ubuntu@$f:/home/ubuntu/
        doSetup $f
done

sleep 50

echo "Metaserver IP to be used for client is : $metaServerIp " 
echo "Client run command : $qfsclientBinaryPath"

#ssh ubuntu@$qfsClientIp 'bash -s' < $qfsclientBinaryPath
ssh ubuntu@$qfsClientIp "$qfsclientBinaryPath"


echo "Giving time populate the cache information from chunkserver to metaserver"
if [ "$mode" == "orig" ]
then 
	sleep 5
elif [ "$mode" == "repair" ]
then
	sleep 40
fi


#for tcmdump from remote machine
#rm /tmp/pipe
#mkfifo /tmp/pipe
#for f in $chunkservers
#do
#        echo "Setting tp TCP dump $f"
#        scp $chunkRunScriptName ubuntu@$f:/home/ubuntu/
#        doTCPDump $f
#done

#toKillChunkServer="192.168.0.250"
toKillChunkServer=""
for s in $chunkservers
do
	echo "Counting chunks: $s"
        res=$(countChunksOnChunkServer $s)
        echo "$s  has : $res"
        #if ((res == numChunksToLose))
        if ((res >= numChunksToLose))
        then
           toKillChunkServer=$s
           break
        fi
done

#echo "Now sleeping for 5 hours!!"
#sleep 18000

echo "Killing Chunk Server : $toKillChunkServer"
ssh ubuntu@$toKillChunkServer 'bash -s' < $chunkKillScriptName

#/home/ubuntu/takePeriodicNetStat.sh  &  # take periodic snapshot of netstat

sleepTime=50
echo "Give time to repair: $sleepTime sec"
sleep $sleepTime



if [ -z "$targetDirForExp" ]; then
        echo "We have not provided copying log information. So Nothing to do further...exiting..."
        exit 
fi

mkdir -p $targetDirForExp

for f in $chunkservers
do
        echo "Copying chunkserver logs $f to $targetDirForExp"
        doMoveChunkServerLogs $f $targetDirForExp
done

metaServerOrigName="/home/ubuntu/qfsbase/meta/MetaServer.log"
metaServerOrigName1="/home/ubuntu/qfsbase/meta/MetaServer.log.1"
metaServerOrigName2="/home/ubuntu/qfsbase/meta/MetaServer.log.2"

metaServerRename="$targetDirForExp/MetaServer.log"
metaServerRename1="$targetDirForExp/MetaServer.log.1"
metaServerRename2="$targetDirForExp/MetaServer.log.2"

mv $metaServerOrigName $metaServerRename
mv $metaServerOrigName1 $metaServerRename1
mv $metaServerOrigName2 $metaServerRename2



