#!/bin/bash
# gvp.sh - collect performance data from Gluster 
# for a particular Gluster volume
# by Ben England, copyright Apache v2
# usage: bash gvp.sh your-gluster-volume sample-count sample-interval
#
volume_name=$1
sample_count=$2
sample_interval=$3
if [ "$sample_interval" = "" ]  ; then
  echo "usage: gvp.sh your-gluster-volume sample-count sample-interval-sec"
  exit 1
fi
gluster volume profile $volume_name start
gluster volume profile $volume_name info > /tmp/past
for min in `seq 1 $sample_count` ; do
  sleep $sample_interval
  gluster volume profile $volume_name info
done > gvp.log
gluster volume profile $volume_name stop
echo "output written to gvp.log"
