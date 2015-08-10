#!/bin/bash

echo "KILLING the metaserver:" 
cat /home/ubuntu/qfsbase/meta/metaserver.pid
cat /home/ubuntu/qfsbase/meta/metaserver.pid | xargs kill
echo "KILLING the WebUI:" 
cat /home/ubuntu/qfsbase/meta/webui.pid 
cat /home/ubuntu/qfsbase/meta/webui.pid | xargs kill
