#!/bin/bash


chunkservers=$(cat "/home/ubuntu/chunk_server_list.txt")

function runMetaServer {

ssh ubuntu@$metaServerIp '/home/ubuntu/kill_meta_server.sh'

metaReleaseDir="$qfsdir/$buildDir/debug/"

metaPath="$qfsdir/examples/sampleservers/sample_setup.py"

metaUninstallScript="$metaPath -r $metaReleaseDir -c /home/ubuntu/sample_setup.cfg -a uninstall"
#$qfsdir/examples/sampleservers/sample_setup.py -r $metaReleaseDir -c /home/ubuntu/sample_setup.cfg -a uninstall
echo "Uninstall script: $metaUninstallScript" 
ssh ubuntu@$metaServerIp $metaUninstallScript

sleep 1


#echo "***   Netstat BEFORE metaserver launch  **** "
#echo "-------------------------------------------- "
#netstat

metaInstallScript="$metaPath -r $metaReleaseDir -c /home/ubuntu/sample_setup.cfg -a install"
#$qfsdir/examples/sampleservers/sample_setup.py -r $metaReleaseDir -c /home/ubuntu/sample_setup.cfg -a install

echo "Install script: $metaInstallScript" 
ssh ubuntu@$metaServerIp $metaInstallScript

#echo "***   Netstat AFTER metaserver launch  **** "
#echo "-------------------------------------------- "
#netstat

}

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
#metaServerIp="192.168.0.235"
metaServerIp="192.168.1.145"
#if [ "$metaServerIp" == ""]
#then
#     metaServerIp="192.168.0.235"
#fi

echo "Metaserver IP to be used for client is : $metaServerIp " 

declare -A waitTimeVsFaults

waitTimeVsFaults=( ["1"]="60" ["2"]="60" ["4"]="120" ["8"]="300" ["16"]="600" ["32"]="700" ["64"]="1000" ["128"]="2200" ["256"]="3600" )

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
#chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33"
#chunkservers="192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149 192.168.1.15 192.168.1.150"
#chunkservers="192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149"  #10 servers for 6+3
#chunkservers="192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149 192.168.1.15 192.168.1.150 192.168.1.56 192.168.1.155"  #14 servers for 12+1 decoding


#listOfFilesToCopy="ChunkServer.prp MetaServer.prp sample_setup.cfg kill_chunk_server_repair.sh kill_chunk_server.sh kill_meta_server.sh launch_chunk_server_repair.sh launch_chunk_server.sh launch_meta_server.sh run_experiment.sh"
listOfFilesToCopy="ChunkServer.prp"

#qfsClientIp=192.168.0.34
qfsClientIp=192.168.1.155
 
#for s in $chunkservers
#do
#	echo "Processing $s"
#	for f in $listOfFilesToCopy
#	do 
#		echo "Copying.. $f"
#        	doSCP $s $f
#        done
#done

doRestart $qfsClientIp
doRestart $metaServerIp

for s in $chunkservers
do
	echo "Rebooting ..  $s"
#        scp $chunkKillScriptName ubuntu@$s:/home/ubuntu/
        doKill $s
        doRestart $s
done
sleep 120

runMetaServer

sleep 50


#chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33"
#chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.27 192.168.0.28 192.168.0.29 192.168.0.3 192.168.0.30 192.168.0.31 192.168.0.32 192.168.0.33"
#chunkservers="192.168.0.246 192.168.0.250 192.168.0.251 192.168.0.252 192.168.0.253 192.168.0.254 192.168.0.26 192.168.0.27 192.168.0.28 192.168.0.29"
#chunkservers="192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149 192.168.1.15 192.168.1.150"
#chunkservers="192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149"
#chunkservers="192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149 192.168.1.15 192.168.1.150 192.168.1.52 192.168.1.56"  #14 servers for 12+1 decoding
#chunkservers="192.168.1.138 192.168.1.139 192.168.1.14 192.168.1.153 192.168.1.141 192.168.1.142 192.168.1.143 192.168.1.144 192.168.1.147 192.168.1.149 192.168.1.15 192.168.1.150 192.168.1.56 192.168.1.155"  #14 servers for 12+1 decoding


for f in $chunkservers
do
        echo "Setting up $f"
#        scp $chunkRunScriptName ubuntu@$f:/home/ubuntu/
        doSetup $f
done

sleep 120

echo "Metaserver IP to be used for client is : $metaServerIp " 
echo "Client run command : $qfsclientBinaryPath"

#ssh ubuntu@$qfsClientIp 'bash -s' < $qfsclientBinaryPath
ssh ubuntu@$qfsClientIp "$qfsclientBinaryPath"

for s in $chunkservers
do
    echo "File in chunk server: $s"
    ssh ubuntu@$s "ls qfsbase/chunkdir11/*.*"
done

#echo -n "Enter when yhou are ready: "
#read takeuserEnter

echo "Giving time populate the cache information from chunkserver to metaserver"
if [ "$mode" == "orig" ]
then 
	sleep 10
elif [ "$mode" == "repair" ]
then
	sleep 100
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

#sleepTime=240
sleepTime=${waitTimeVsFaults[$numChunksToLose]}
echo "Give time to repair: $sleepTime sec"
sleep $sleepTime



if [ -z "$targetDirForExp" ]; then
        echo "We have not provided copying log information. So Nothing to do further...exiting..."
        exit 
fi

mkdir -p $targetDirForExp

#for f in $chunkservers
#do
#        echo "Copying chunkserver logs $f to $targetDirForExp"
#        doMoveChunkServerLogs $f $targetDirForExp
#done

metaServerOrigName="/home/ubuntu/qfsbase/meta/MetaServer.log"
metaServerOrigName1="/home/ubuntu/qfsbase/meta/MetaServer.log.1"
metaServerOrigName2="/home/ubuntu/qfsbase/meta/MetaServer.log.2"
metaServerOrigName3="/home/ubuntu/qfsbase/meta/MetaServer.log.3"
metaServerOrigName4="/home/ubuntu/qfsbase/meta/MetaServer.log.4"
metaServerOrigNameAll="/home/ubuntu/qfsbase/meta/MetaServer.*"

metaServerRename="$targetDirForExp/MetaServer.log"
metaServerRename1="$targetDirForExp/MetaServer.log.1"
metaServerRename2="$targetDirForExp/MetaServer.log.2"
metaServerRename3="$targetDirForExp/MetaServer.log.3"
metaServerRename4="$targetDirForExp/MetaServer.log.4"
metaServerRenameAll="$targetDirForExp/"

scp ubuntu@metaServerIp:$metaServerOrigName $metaServerRename
scp ubuntu@metaServerIp:$metaServerOrigName1 $metaServerRename1
scp ubuntu@metaServerIp:$metaServerOrigName2 $metaServerRename2
scp ubuntu@metaServerIp:$metaServerOrigName3 $metaServerRename3
scp ubuntu@metaServerIp:$metaServerOrigName4 $metaServerRename4
scp ubuntu@metaServerIp:$metaServerOrigNameAll $metaServerRenameAll
#mv $metaServerOrigName $metaServerRename
#mv $metaServerOrigName1 $metaServerRename1
#mv $metaServerOrigName2 $metaServerRename2
#mv $metaServerOrigName3 $metaServerRename3
#mv $metaServerOrigName4 $metaServerRename4
#mv $metaServerOrigNameAll $metaServerRenameAll



