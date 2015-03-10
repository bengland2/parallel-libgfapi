#!/bin/bash
# execute a command in parallel on a set of host specified in param 1
# you must quote the command if it's more than a single word
# parameter 1: file containing list of hostnames/IPs, 1 host per record
# parameter 2: the command to execute, quote the command if it's more than a s

logdir=/tmp/par-for-all.$$
echo "log directory is $logdir"
cmd="$2"
hostlist=$1
OK=0
SSH="ssh -o StrictHostKeyChecking=no -nTx "
#echo "starting in parallel on : "
rm -rf $logdir && mkdir -p $logdir
for n in `cat $hostlist` ; do 
  #echo -n " $n" 
  if [ -n "$REMOTE_USER" ] ; then remuser="${REMOTE_USER}@" ; fi
  eval "$SSH ${remuser}$n \"$cmd\" > $logdir/$n.log 2>&1 &"
  pids="$pids $!"
  mydir=`dirname $0`
  # throttle the launching of parallel ssh threads so we don't break ssh
  ( echo 'import time' ; echo 'time.sleep(0.1)' ) | python
done
echo
j=0
host_array=( `cat $hostlist` )
for p in $pids ; do 
  h=${host_array[$j]}
  wait $p
  s=$?
  chars=`wc -c < $logdir/$h.log`
  if [ $chars -ge 1 -o $s != $OK ] ; then
    echo
    echo "--- $h ---"
    if [ $s != $OK ] ; then
      echo "pid $p on host $h returns $s"
    fi
    cat $logdir/$h.log
  fi
  retcodes="$retcodes $s"
  (( j = $j + 1 ))
done
for s in $retcodes ; do
  if [ $s != $OK ] ; then exit $s ; fi
done
exit $OK

