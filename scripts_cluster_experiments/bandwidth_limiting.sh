#!/bin/bash

TC=tc

#IF=$2             # Interface
IF="eth0"             # Interface

DNLD=$2          # DOWNLOAD Limit

UPLD=$2          # UPLOAD Limit

# IP address of the machine we are controlling
#IP=$(ifconfig | grep "inet addr" | sed 's|addr:||' | awk '/Bcast/ {print $2;}')
IP=$3


#Burst
#BURST=$4

start() {

#BURST=$(echo "$UPLD * 1000000 * 0.00025" | bc)
BURST=$(echo "$UPLD * 1048576 * 0.00025" | bc)

#rate=$(echo "$UPLD * 1000000" | bc)
rate=$(echo "$UPLD * 1048576" | bc)
UPLD=$rate
DNLD=$rate

echo "Starting IP= $IP, IF= $IF, rate= $UPLD, BURST= $BURST"
$TC qdisc add dev $IF root handle 1: htb 
$TC qdisc add dev $IF ingress handle ffff


$TC class add dev $IF parent 1: classid 1:1 htb rate $UPLD burst $BURST
$TC filter add dev $IF protocol ip parent 1:0 prio 1 u32 match ip src $IP flowid 1:1

tc filter add dev $IF parent ffff: protocol ip u32 match ip dst $IP police rate $DNLD burst $BURST drop flowid 2:

iptables -A INPUT -t mangle -j MARK --set-mark 2
iptables -A OUTPUT -t mangle -j MARK --set-mark 1 



}

stop() {
echo "Stopping IF= $IF "
# Stop the bandwidth shaping.
$TC qdisc del dev $IF root
$TC qdisc del dev $IF ingress

}

restart() {

stop
sleep 1
start

}

show() {

# Display status of traffic control status.
$TC -s qdisc ls dev $IF

}

change() {
echo -n "changing to $UPLD"
$TC class change dev $IF parent 1: classid 1:1 htb rate $UPLD burst $BURST
}

case "$1" in

start)

echo -n "Starting bandwidth shaping: "
start
echo "done"
;;

stop)

echo -n "Stopping bandwidth shaping: "
stop
echo "done"
;;

restart)

echo -n "Restarting bandwidth shaping: "
restart
echo "done"
;;

show)

echo "Bandwidth shaping status for $IF:"
show
echo ""
;;

change)
#echo -n "Changing"
change
echo ""
;;

*)

pwd=$(pwd)
#echo "Usage: ./bandwidth_limiting.sh {start|stop|restart|show|change} <interface> <rate> <burst>"
echo "Usage: ./bandwidth_limiting.sh {start|stop|restart|show|change} <rate> <ipaddr>"
echo "Burst = rate in bytes * 0.00025"
;;

esac 
exit 0

