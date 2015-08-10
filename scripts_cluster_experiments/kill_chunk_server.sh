#!/bin/bash

echo "KILLING the chunkserver:" 
cat  qfsbase/chunkserver.pid
cat qfsbase/chunkserver.pid | xargs kill
rm -rf qfsbase/
