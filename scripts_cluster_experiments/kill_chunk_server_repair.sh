#!/bin/bash

echo "KILLING the chunkserver:" 
rm -rf qfsbase/
cat  qfsbase/chunkserver.pid
cat qfsbase/chunkserver.pid | xargs kill
