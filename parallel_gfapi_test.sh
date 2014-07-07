#!/bin/bash
#
# parallel_gfapi_test.sh - script to run parallel gfapi_perf_test processes across a set of client hosts
# 
# this script depends on par-for-all.sh , a script that launches a command in parallel across a set of hosts
# specified in a text file.
# 
# environment variables:
# PGFAPI_PROCESSES - how many processes per client (default: 16)
# PGFAPI_THREADS_PER_PROC - how many POSIX threads per process (default: 1)
# PGFAPI_FILES - number of files per thread to use (default 10000)
# PGFAPI_LOAD - what kind of workload - seq-wr or seq-rd, default is seq-wr 
#               "seq" = "sequential", "wr" = "write", "rd" = "read", 
# PGFAPI_FUSE - defaults to 0, if 1 use equivalent POSIX fs calls instead of libgfapi
# PGFAPI_DIRECT - defaults to 0, if 1 use O_DIRECT flag at open time
# PGFAPI_TOPDIR - top directory used within the Gluster volume, default is /tmp
# PGFAPI_CLIENTS - filename containing list of clients
# PGFAPI_APPEND - defaults to 0, if 1 then append to file don't create it
# PGFAPI_OVERWRITE - defaults to 0, if 1 then overwrite existing file don't create it
# PGFAPI_FILESIZE - defaults to 4 (KB), number of KB to write or read per file
# PGFAPI_EXTERNAL_START - if defined, then then let user fire the starting gun 
#                           (allows multiple concurrent parallel_gfapi_test.sh runs)
#
#threads=16
filesize_kb=${PGFAPI_FILESIZE:-4}
processes=${PGFAPI_PROCESSES:-4}
files=${PGFAPI_FILES:-10240}
recordsize_kb=${PGFAPI_RECORDSIZE:-64}
clientFile=${PGFAPI_CLIENTS:-clients.list}
export GFAPI_VOLNAME=alu-jbod3
export GFAPI_HOSTNAME=gprfs045-10ge
export GFAPI_LOAD=${PGFAPI_LOAD:-seq-wr}
export GFAPI_FUSE=${PGFAPI_FUSE:-0}
export GFAPI_APPEND=${PGFAPI_APPEND:-0}
export GFAPI_OVERWRITE=${PGFAPI_OVERWRITE:-0}
export GFAPI_USEC_DELAY_PER_FILE=${PGFAPI_USEC_DELAY_PER_FILE:-1000}
export GFAPI_STARTING_GUN_TIMEOUT=120
export GFAPI_FSYNC_AT_CLOSE=${PGFAPI_FSYNC_AT_CLOSE:-0}
export GFAPI_RDPCT=${PGFAPI_RDPCT:-0}
export GFAPI_THREADS_PER_PROC=${PGFAPI_THREADS_PER_PROC:-1}
MOUNTPOINT=/mnt/alu-jbod3
TOPDIR=${PGFAPI_TOPDIR:-/smf-gfapi}
# if you want to use Gluster mountpoint as common directory that's ok
PROGRAM=/root/gfapi_perf_test
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
echo "processes per client: $processes"
echo "threads per process: $GFAPI_THREADS_PER_PROC"
echo "test driver glusterfs mountpoint: $MOUNTPOINT"
echo "top directory within Gluster volume: $TOPDIR"
echo "each thread (process) runs program at: $PROGRAM"
if [ $GFAPI_FUSE = 1 ] ; then 
  echo "using Gluster FUSE mountpoints on each client"
fi
if [ $GFAPI_DIRECT = 1 ] ; then 
  echo "using direct I/O"
fi
if [ $GFAPI_OVERWRITE = 1 ] ; then
  echo "overwriting existing files"
fi
if [ $GFAPI_APPEND = 1 ] ; then
  echo "appending to existing files"
fi
if [ "$GFAPI_LOAD" = "rnd-wr" -o "$GFAPI_LOAD" = "rnd-rd" ] ; then
  echo "I/O requests per thread: $GFAPI_IOREQ"
fi
clients="`cat $clientFile`"
clientCnt=`cat $clientFile | wc -l `
# if you want to use Gluster mountpoint as log directory, that's ok
# PGFAPI_LOGDIR=$MOUNTPOINT/$TOPDIR/glfs_smf_logs
export PGFAPI_LOGDIR=${TMPDIR:-/tmp}/parallel_gfapi_logs.$$
echo "log files for each libgfapi process at $PGFAPI_LOGDIR"

(( start_gun_timeout = $clientCnt * $processes * $GFAPI_THREADS_PER_PROC * 3 / 10 ))
(( start_gun_timeout = $start_gun_timeout + 10 ))
export GFAPI_STARTING_GUN_TIMEOUT=$start_gun_timeout
echo "starting gun timeout = $GFAPI_STARTING_GUN_TIMEOUT"

starting_gun=${PGFAPI_EXTERNAL_START:-$TOPDIR/start.tmp}
if [ "$GFAPI_FUSE" = 0 ] ; then
  GFAPI_STARTING_GUN=$starting_gun
else
  GFAPI_STARTING_GUN=${MOUNTPOINT}/$starting_gun
  GFAPI_BASEDIR=${MOUNTPOINT}/$GFAPI_BASEDIR
fi

OK=0
NOTOK=1

pace() {
  ( echo "import time" ; echo "time.sleep($1)" ) | python
}

# create empty directory tree

mkdir -p $MOUNTPOINT/$TOPDIR
find $MOUNTPOINT/$TOPDIR -maxdepth 1 -name '*.ready' -delete
rm -f $MOUNTPOINT/$TOPDIR/$GFAPI_STARTING_GUN
find $MOUNTPOINT/$TOPDIR -maxdepth 1 -name '*.*.log' -delete
rm -f $MOUNTPOINT/$starting_gun

ALL_LOGS_DIR=$PGFAPI_LOGDIR
rm -rf $ALL_LOGS_DIR
mkdir -p $ALL_LOGS_DIR


# if write test then remove files from each per-thread directory tree in parallel

echo "removing any previous files"
if [ "$GFAPI_LOAD" = "seq-wr" -a "$GFAPI_APPEND" = "0" -a "$GFAPI_OVERWRITE" = 0 ] ; then
 thrdcnt=0
 for c in $clients ; do
  ssh $c 'killall -INT -q rm ; sleep 1 ; killall -q rm'
  for n in `seq -f "%02g" 1 $processes ` ; do 
   d=$TOPDIR/smf-gfapi-${c}.$n
   if [ "$GFAPI_FUSE" = 1 ] ; then
     d=${MOUNTPOINT}$d
   fi
   glfs_cmd="GFAPI_LOAD=unlink GFAPI_FUSE=$GFAPI_FUSE GFAPI_FILES=$files GFAPI_BASEDIR=$d GFAPI_VOLNAME=$GFAPI_VOLNAME GFAPI_HOSTNAME=$GFAPI_HOSTNAME GFAPI_THREADS_PER_PROC=$GFAPI_THREADS_PER_PROC $PROGRAM"
   
   eval "$glfs_cmd > /tmp/unlink.$c.$n.log 2>&1 &"
   rmpids="$rmpids $!"
   (( thrdcnt = $thrdcnt + 1 ))
   if [ $thrdcnt -gt 25 ] ; then
     for p in $rmpids ; do wait $p ; done
     rmpids=""
     thrdcnt=0
   fi
  done
  ssh $c "rm -f /tmp/glfs-*.log /tmp/unlink.*.*.log"
 done
 for p in $rmpids ; do wait $p ; done
 rm -f $TOPDIR/*.ready 
fi
par-for-all.sh servers.list 'sync'
sleep 2
export GFAPI_STARTING_GUN

# start the threads

echo -n "`date`: starting $clientCnt clients ... "
for c in $clients ; do
 for n in `seq -f "%02g" 1 $processes` ; do 
  mkdir -p ${MOUNTPOINT}$TOPDIR/smf-gfapi-${c}.$n
 done
done
sleep 2
for c in $clients ; do
 echo -n "$c "
 for n in `seq -f "%02g" 1 $processes` ; do 
  d=$TOPDIR/smf-gfapi-${c}.$n
  if [ "$GFAPI_FUSE" != 1 ] ; then
    export GFAPI_BASEDIR=$d
  else
    export GFAPI_BASEDIR=${MOUNTPOINT}$d
  fi
  export GFAPI_RECSZ=$recordsize_kb
  export GFAPI_FSZ=${filesize_kb}k
  export GFAPI_FILES=$files
  glfs_cmd="GFAPI_STARTING_GUN=$GFAPI_STARTING_GUN GFAPI_STARTING_GUN_TIMEOUT=$GFAPI_STARTING_GUN_TIMEOUT GFAPI_LOAD=$GFAPI_LOAD GFAPI_USEC_DELAY_PER_FILE=$GFAPI_USEC_DELAY_PER_FILE GFAPI_RECSZ=$GFAPI_RECSZ GFAPI_FSZ=$GFAPI_FSZ GFAPI_FILES=$GFAPI_FILES GFAPI_BASEDIR=$GFAPI_BASEDIR GFAPI_FSYNC_AT_CLOSE=$GFAPI_FSYNC_AT_CLOSE GFAPI_FUSE=$GFAPI_FUSE GFAPI_VOLNAME=$GFAPI_VOLNAME GFAPI_HOSTNAME=$GFAPI_HOSTNAME GFAPI_RDPCT=$GFAPI_RDPCT GFAPI_THREADS_PER_PROC=$GFAPI_THREADS_PER_PROC $PROGRAM"
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
  pace 0.1
 done
done 
echo 

# wait for threads to reach starting gate

(( totalThreads = $processes * $GFAPI_THREADS_PER_PROC * $clientCnt ))
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

if [ -z $PGFAPI_EXTERNAL_START ] ; then 
  touch $MOUNTPOINT/$starting_gun
else
  echo "waiting for external starting gun $PGFAPI_EXTERNAL_START ..."
  while [ ! -f $MOUNTPOINT/$starting_gun ] ; do sleep 3 ; done
fi
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
      '{ threads += 1 ; if ($3 != "") { total_mbs += $2; total_fps+=$3; total_iops+=$4; threads_done+=1}}END{printf "%d threads finished out of %d \ntransfer-rate: %6.2f MBytes/s\nfile-rate: %8.2f files/sec\nIOPS:%8.2f requests/sec\n\n", threads_done, threads, total_mbs, total_fps, total_iops }'

exit $status
