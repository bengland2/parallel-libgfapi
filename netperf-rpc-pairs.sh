#!/bin/bash
# WARNING: DO NOT RUN THIS ON SYSTEM WHILE IT IS IN USE!!!
#
# netperf-rpc-pairs.sh - simulate RPC-like request-response protocol
# using netperf TCP_RR
#
# command line parameters:
#
# - sender-list - file containing hosts to initiate requests, 1 host/record
# - receiver-list - file containing hosts to receive requests, "
# - thread-count - how many concurrent requests to issue = netperf threads
#
# environment variables:
#
# TEST_TIME - how many seconds to run the test for
# SEND_SIZE - how big a message to send in request
# RCV_SIZE - how big a message will be sent in response
# 

# introduce a slight pause so that we don't overdrive ssh and cause errors
# input parameter is max number of events/sec to allow

function pace() {
        ( echo import time ; echo "time.sleep(1.0/$1)" ) | python
}

usage() {
  echo "$1"
  echo "usage: netperf-pairs.sh sender-list receiver-list [ thread-count ]"
  exit $NOTOK 
}

NOTOK=1
OK=0
MILLION=1000000
BITS_PER_BYTE=8
(( BYTES_PER_MEGABYTE = 1024 * 1024 ))
SSH="ssh -nx -o StrictHostKeyChecking=no "
SCP="scp -Bq -o StrictHostKeyChecking=no "

# parse command line inputs  and display test parameters

sender_list=$1
receiver_list=$2
threads=$3
if [ "$sender_list" = ""  -o "$receiver_list" = "" ] ; then 
  usage "missing command line parameters"
fi
if [ "$TEST_TIME" = "" ] ; then TEST_TIME=30 ; fi
if [ "$SEND_SIZE" = "" ] ; then SEND_SIZE=512 ; fi
if [ "$RCV_SIZE" = "" ] ; then RCV_SIZE=131072 ; fi
echo "sender list = $sender_list"
echo "receiver list = $receiver_list"
echo "test time = $TEST_TIME seconds"
echo "send size = $SEND_SIZE bytes"
echo "receive size = $RCV_SIZE bytes"

list="`cat $sender_list`"
sender=( $list )
list="`cat $receiver_list`"
receiver=( $list )
count=${#sender[*]}
unique_receivers=`for n in ${receiver[*]} ; do echo $n ; done | sort -u`
#echo "netserver hosts in use: $unique_receivers"
unique_senders=`for n in ${sender[*]} ; do echo $n ; done | sort -u`
#echo "netperf hosts in use: $unique_senders"
if [ -z "$threads" ] ; then threads=`wc -l < $sender_list` ; fi
if [ $threads -lt $count ] ; then count=$threads ; fi
if [ $count = 0 ] ; then
  usage "not enough hosts in $sender_list"
fi
if [ `wc -l $sender_list` != `wc -l $receiver_list` ] ; then
  usage "$sender_list must have same number of records as $receiver_list"
fi
echo 'WARNING: DO NOT RUN THIS ON SYSTEM WHILE IT IS IN USE!!!'

# use this to clean up from error or caught signal

abort()
{
 i=0
 for h in $sender_list ; do 
	$SSH $h killall -INT netperf
 done

 if [ "$pids" != "" ] ; then
  kill -KILL $pids
 fi
}

trap abort 1 2 3

echo "starting netserver on target servers where necessary: "
for h in ${unique_receivers[*]} ; do
	echo -n " $h"
	$SSH $h "netperf -l 1 -L ${h} -H ${h} -t TCP_RR -- -r $SEND_SIZE,$RCV_SIZE \
	  > /tmp/np$$.log || netserver -L ${h} > /tmp/nps-present-$$.log 2>&1"  &
	netserver_pids="$netserver_pids $!"
	pace 10
done
echo
for p in $netserver_pids ; do 
  wait $p
done

(cd /tmp ; rm -f netperf*.log netperf-all.log n.tmp sum.tmp sum_MBps.tmp)

echo "starting netperf threads at `date`"
i=0
thrds_per_sec=10
s=0
(( sleep_time = $count / $thrds_per_sec ))
(( sleep_time = $sleep_time + 2 ))
while [ $i -lt $count ] ; do
	next_receiver=${receiver[$i]}
	next_sender=${sender[$i]}
	echo "starting up netperf on sender $next_sender to receiver $next_receiver"
        if [ $next_sender = $next_receiver ] ; then
		echo "WARNING: sender = receiver, will not go across network"
	fi
	logfn="/tmp/netperf_${i}_from_${next_sender}_to_$next_receiver.log"
	netperf_cmd="netperf -l $TEST_TIME -L ${next_sender} -H ${next_receiver} "
	netperf_cmd="$netperf_cmd -t TCP_RR -- -r $SEND_SIZE,$RCV_SIZE"
	echo "launching: $netperf_cmd"
	$SSH $next_sender "sleep $sleep_time  ; $netperf_cmd ; date" > $logfn &
	pids="$pids $!"
	(( i = $i + 1 ))
	(( s = $s + 1 ))
	if [ $s -ge $thrds_per_sec ] ; then
		s=0
		(( sleep_time = $sleep_time - 1 ))
	fi
	pace $thrds_per_sec
done

echo "waiting for netperfs to complete at `date`  ..."
result=$OK
for p in $pids ; do
	wait $p
	r=$?
	if [ $r != 0 ] ; then
		echo "pid $p exit status $r"
		result=$r
	fi
done
if [ $result != $OK ] ; then 
	echo "ERROR: one of clients did not complete successfully, check logs"
	exit $NOTOK
fi

cd /tmp
echo "netperfs all done at `date`"
echo "per-client results available in /tmp"
#for f in netperf_*_from_*_to_*.log ; do 
#  ( echo "" ; echo $f ; cat $f ) >> netperf-all.log
#done
#cat netperf-all.log

# extract each netperf's transfer rate in millions of bits / second

for f in netperf_*_from_*_to_*.log ; do grep "$TEST_TIME\.00" $f | tail -2 ; done > n.tmp

# add them together to get aggregate transfer rate in millions of bits/second

touch sent_sum.tmp rcvd_sum.tmp
#awk '/16384/' n.tmp
awk -v BYTES_PER_MB=$BYTES_PER_MEGABYTE \
  'BEGIN{sum=0}{ if (NF == 6) sum += (($6*$4)/BYTES_PER_MB)}END{printf "%7.2f\n", sum}' \
  n.tmp > rcvd_sum.tmp
awk -v BYTES_PER_MB=$BYTES_PER_MEGABYTE \
  'BEGIN{sum=0}{ if (NF == 6) sum += (($6*$3)/BYTES_PER_MB)}END{printf "%7.2f\n", sum}' \
  n.tmp > sent_sum.tmp
if [ "`wc -l sent_sum.tmp`" = 0 ] ; then
	echo "netperf processes did not run"
	exit $NOTOK
fi

# convert to megabytes/sec

echo "aggregate transmit rate = `cat sent_sum.tmp` megabytes/sec"
echo "aggregate receive rate = `cat rcvd_sum.tmp` megabytes/sec"
