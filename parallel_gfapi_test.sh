#!/bin/bash
#
# parallel_gfapi_test.sh - script to run parallel gfapi_perf_test processes across a set of client hosts
# 
# environment variables:
# PGFAPI_THREADS - how many threads per client (default: 16)
# PGFAPI_FILES - number of files per thread to use (default 10000)
# PGFAPI_LOAD - what kind of workload - seq-wr or seq-rd, default is seq-wr 
#               "seq" = "sequential", "wr" = "write", "rd" = "read", 
# PGFAPI_FUSE - defaults to 0, if 1 use equivalent POSIX fs calls instead of libgfapi
# PGFAPI_DIRECT - defaults to 0, if 1 use O_DIRECT flag at open time
# PGFAPI_TOPDIR - top directory used within the Gluster volume, default is /tmp
#
#threads=16
threads=${PGFAPI_THREADS:-4}
files=${PGFAPI_FILES:-10240}
filesize_kb=4
recordsize_kb=4
clientFile=clients.list
#clientFile=1cl.list
export GFAPI_VOLNAME=scale
export GFAPI_HOSTNAME=172.17.50.2
export GFAPI_LOAD=${PGFAPI_LOAD:-seq-wr}
export GFAPI_FUSE=${PGFAPI_FUSE:-0}
export GFAPI_APPEND=${PGFAPI_APPEND:-0}
export GFAPI_OVERWRITE=${PGFAPI_OVERWRITE:-0}
MOUNTPOINT=/mnt/scale
TOPDIR=${PGFAPI_TOPDIR:-/smf-gfapi}
# if you want to use Gluster mountpoint as common directory that's ok
PER_THREAD_PROGRAM=/root/gfapi_perf_test
export GFAPI_DIRECT=${PGFAPI_DIRECT:-0}
export GFAPI_IOREQ=4096
#
# NO EDITABLE PARAMETERS BELOW THIS LINE
#
echo "volume name: $GFAPI_VOLNAME"
echo "Gluster server in the volume: $GFAPI_HOSTNAME"
echo "workload: $GFAPI_LOAD"
echo "list of clients in file: $clientFile"
echo "record size (KB): $recordsize_kb"
echo "file size (KB): $filesize_kb"
echo "files per thread: $files"
echo "threads per client: $threads"
echo "test driver glusterfs mountpoint: $MOUNTPOINT"
echo "top directory within Gluster volume: $TOPDIR"
echo "each thread (process) runs program at: $PER_THREAD_PROGRAM"
if [ $GFAPI_FUSE = 1 ] ; then 
  echo "using Gluster FUSE mountpoints on each client"
fi
if [ $GFAPI_DIRECT = 1 ] ; then 
  echo "using direct I/O"
fi
if [ "$GFAPI_LOAD" = "rnd-wr" -o "$GFAPI_LOAD" = "rnd-rd" ] ; then
  echo "I/O requests per thread: $GFAPI_IOREQ"
fi
clients="`cat $clientFile`"
clientCnt=`cat $clientFile | wc -l `
# if you want to use Gluster mountpoint as log directory, that's ok
# PGFAPI_LOGDIR=$MOUNTPOINT/$TOPDIR/glfs_smf_logs
export PGFAPI_LOGDIR=${TMPDIR:-/tmp}/parallel_gfapi_logs
echo "log files for each libgfapi process at $PGFAPI_LOGDIR"

starting_gun=start.tmp
if [ "$GFAPI_FUSE" = 0 ] ; then
  export GFAPI_STARTING_GUN=$TOPDIR/$starting_gun
else
  export GFAPI_STARTING_GUN=${MOUNTPOINT}$TOPDIR/$starting_gun
fi
(( start_gun_timeout = $clientCnt * $threads * 3 / 10 ))
(( start_gun_timeout = $start_gun_timeout + 10 ))
export GFAPI_STARTING_GUN_TIMEOUT=$start_gun_timeout

# create empty directory tree

OK=0
mkdir -p $MOUNTPOINT/$TOPDIR
find $MOUNTPOINT/$TOPDIR -maxdepth 1 -name '*.ready' -delete
rm -f $MOUNTPOINT/$TOPDIR/$GFAPI_STARTING_GUN
find $MOUNTPOINT/$TOPDIR -maxdepth 1 -name '*.*.log' -delete
rm -f $MOUNTPOINT/$TOPDIR/$starting_gun

ALL_LOGS_DIR=$PGFAPI_LOGDIR
rm -rf $ALL_LOGS_DIR
mkdir -p $ALL_LOGS_DIR


# if write test then remove files from each per-thread directory tree in parallel

if [ "$GFAPI_LOAD" = "seq-wr" -a "$GFAPI_APPEND" = "0" -a "$GFAPI_OVERWRITE" = 0 ] ; then
 for c in $clients ; do
  for n in `seq -f "%02g" 1 $threads` ; do 
   d=$TOPDIR/smf-gfapi-${c}.$n
   eval "rm -rf $MOUNTPOINT/$d &"
   rmpids="$rmpids $!"
  done
 done
 for p in $rmpids ; do wait $p ; done
 rm -f $TOPDIR/*.ready
fi
sync
sleep 2

# start the threads

echo -n "`date`: starting $clientCnt clients ... "
for c in $clients ; do
 echo -n "$c "
 for n in `seq -f "%02g" 1 $threads` ; do 
  d=$TOPDIR/smf-gfapi-${c}.$n
  if [ "$GFAPI_FUSE" = 0 ] ; then
    export GFAPI_BASEDIR=$d
  else
    export GFAPI_BASEDIR=${MOUNTPOINT}$d
  fi
  export GFAPI_RECSZ=$recordsize_kb
  export GFAPI_FSZ=${filesize_kb}k
  export GFAPI_FILES=$files
  mkdir -p $MOUNTPOINT/$d
  glfs_cmd="GFAPI_STARTING_GUN=$GFAPI_STARTING_GUN GFAPI_STARTING_GUN_TIMEOUT=$GFAPI_STARTING_GUN_TIMEOUT GFAPI_LOAD=$GFAPI_LOAD GFAPI_RECSZ=$GFAPI_RECSZ GFAPI_FSZ=$GFAPI_FSZ GFAPI_FILES=$GFAPI_FILES GFAPI_BASEDIR=$GFAPI_BASEDIR GFAPI_VOLNAME=$GFAPI_VOLNAME GFAPI_HOSTNAME=$GFAPI_HOSTNAME $PER_THREAD_PROGRAM"
  if [ -n "$GFAPI_APPEND" ] ; then
    glfs_cmd="GFAPI_APPEND=$GFAPI_APPEND $glfs_cmd"
  fi
  if [ -n "$GFAPI_OVERWRITE" ] ; then
    glfs_cmd="GFAPI_OVERWRITE=$GFAPI_OVERWRITE $glfs_cmd"
  fi
  if [ -n "$GFAPI_DIRECT" ] ; then
    glfs_cmd="GFAPI_DIRECT=$GFAPI_DIRECT $glfs_cmd"
  fi
  if [ -n "$GFAPI_IOREQ" ] ; then
    glfs_cmd="GFAPI_IOREQ=$GFAPI_IOREQ $glfs_cmd"
  fi
  if [ -n "$GFAPI_FUSE" ] ; then
    glfs_cmd="GFAPI_FUSE=$GFAPI_FUSE $glfs_cmd"
  fi
  eval "ssh -o StrictHostKeyChecking=no $c '$glfs_cmd' > $ALL_LOGS_DIR/$c.t$n.log 2>&1 &" 
  pids="$pids $!"
  echo 'import time ; time.sleep(0.1)' | python
 done
done 
echo 

# wait for threads to reach starting gate

(( totalThreads = $threads * $clientCnt ))
while [ 1 ] ; do 
  sleep 1
  threadsReady=`ls $MOUNTPOINT/$TOPDIR/*.ready | wc -l`
  if [ $threadsReady = $totalThreads ] ; then 
    break
  fi
  for p in $pids ; do 
    if [ ! -x /proc/$p ] ; then 
      echo "aborting test because process $p exited prior to the starting gun"
      for p in $pids ; do 
        if [ -x /proc/$p ] ; then kill $p ; fi
      done
      exit 1
    fi
  done
done
echo "`date`: clients are all ready"

# start the test and wait for it to end

touch $MOUNTPOINT/$TOPDIR/$starting_gun
echo "`date` : clients should all start running within a few seconds"
status=$OK
for p in $pids ; do
  wait $p
  s=$?
  if [ $s != $OK ] ; then
    echo "process $p exited with status $s"
    status=$s
  fi
done
echo "`date`: clients completed"
if [ $status != $OK ] ; then
  echo "ERROR: at least one process exited with error status $status"
fi

# report results

( echo "elapsed-sec MBps Files-per-sec I/O-rq-per-sec" ; \
  for f in $ALL_LOGS_DIR/*.log ; do \
    awk '/elapsed time/{t=$4}/throughput/{mbs=$3}/file rate/{fps=$4}/IOPS/{iops=$3}END{print t, mbs,fps,iops}' $f ; \
  done ) > $ALL_LOGS_DIR/result.csv
echo "per-thread results in $ALL_LOGS_DIR/result.csv"
tail -n +2 $ALL_LOGS_DIR/result.csv | awk \
      '{total_mbs += $2; total_fps+=$3; total_iops+=$4; threads+=1}END{printf "transfer-rate: %6.2f MBytes/s\nfile-rate: %8.2f files/sec\nIOPS:%8.2f requests/sec\n\n", total_mbs, total_fps, total_iops }'

exit $status
