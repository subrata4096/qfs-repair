#!/bin/bash

i="0"

rm -rf /home/ubuntu/netstat.log


while [ $i -lt 4 ] #always
do
echo "----------------------------------------------------------------" >> /home/ubuntu/netstat.log
netstat >> /home/ubuntu/netstat.log

sleep 3

done
