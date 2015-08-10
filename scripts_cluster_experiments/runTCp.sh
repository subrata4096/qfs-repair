#!/bin/bash

ssh 192.168.0.26 "sudo tcpdump -i eth0 -s 0 -U -w w.cap not port 22" 
