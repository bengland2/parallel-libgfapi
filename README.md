parallel-libgfapi
=================

benchmark for distributed, multi-thread test of Gluster libgfapi performance

This page describes a new benchmark for measuring small-file performance with libgfapi.  As libgfapi is increasingly used for Gluster applications, questions have arisen about metadata-intensive workload performance with libgfapi and how this scales with bricks, libgfapi instances, etc.   This benchmark takes advantage of the similarity between libgfapi and POSIX interfaces so that the same tests can be run either with libgfapi or in a POSIX filesystem (example: Gluster FUSE mountpoint).  This makes it easy to compare libgfapi performance with FUSE or XFS performance, for example.

The "C" program that runs in each thread is glfs_smf.c .  You can run this program by itself.  The command line syntax is a little odd because it uses environment variables.   This is less user-friendly but also easier in some ways.   For example, you don't have to keep entering the same parameters.

# gcc -g -O0  -Wall --pedantic -o glfs_smf.x86_64 glfs_smf.c -I /usr/include/glusterfs/api  -lgfapi -lrt
# ./glfs_smf.x86_64

getenv_str: you must define environment variable GFAPI_VOLNAME
usage: ./glfs_smf
environment variables may be inserted at front of command or exported
defaults are in parentheses
DEBUG (0 means off)     - print everything the program does
GFAPI_VOLNAME           - Gluster volume to use
GFAPI_HOSTNAME          - Gluster server participating in the volume
GFAPI_TRANSPORT (tcp)   - transport protocol to use, can be tcp or rdma
GFAPI_PORT (24007)      - port number to connect to
GFAPI_RECSZ (64)        - I/O transfer size (i.e. record size) to use
GFAPI_FSZ (1M)          - file size 
GFAPI_BASEDIR(/tmp)     - directory for this thread to use
GFAPI_LOAD (seq-wr)     - workload to apply, can be one of seq-wr, seq-rd, rnd-wr, rnd-rd, unlink
GFAPI_IOREQ (0 = all)   - for random workloads , how many requests to issue
GFAPI_DIRECT (0 = off)  - force use of O_DIRECT even for sequential reads/writes
GFAPI_FUSE (0 = false)  - if true, use POSIX (through FUSE) instead of libgfapi
GFAPI_TRCLVL (0 = none) - trace level specified in glfs_set_logging
GFAPI_FILES (100)       - number of files to access
GFAPI_STARTING_GUN (none) - touch this file to begin test after all processes are started
GFAPI_STARTING_GUN_TIMEOUT (60) - each thread waits this many seconds for starting gun file before timing out
GFAPI_FILES_PER_DIR (1000) - maximum files placed in a leaf directory

[root@gprfc032-10ge gfapi]# GFAPI_VOLNAME=demo GFAPI_HOSTNAME=gprfs024-10ge GFAPI_BASEDIR=/mytmpdir ./glfs_smf.x86_64

GLUSTER: vol=py-gfapi xport=tcp host=gprfs036-10ge port=24007 fuse?No trc.lvl.=0
WORKLOAD: type = seq-wr , file name = /mytmpdir , file size = 1024 KB, file count = 100, record size = 64 KB
file transfers  = 100
I/O (record) transfers = 1600
total bytes = 104857600
elapsed time    = 0.54      sec
throughput      = 185.69    MB/sec
file rate       = 185.69    files/sec
IOPS            = 2971.10   (sequential write)
 
In this program, it creates subdirectories and puts no more than GFAPI_FILES_PER_DIR files in each subdirectory.   This allows you to create more files per thread.  

FIXME: It needs an extra level of directories to run really long tests.


The parallel_gfapi.sh script launches a multi-threaded, distributed test using the above program.  Someday it may switch to using fio with the libgfapi "engine" developed by Huamin Chen, but for now it's simpler to do it this way.  Environment variables supported by this script are:

PSMF_THREADS - how many processes running glfs_smf.c program per client host
PSMF_FILES - how many files should each thread process?
PSMF_FUSE - boolean (0 or 1), defaults to 0, if 1 then use equivalent POSIX system calls
PSMF_TOPDIR - the base directory for the test
PSMF_DIRECT - boolean, default is 0, if 1 then use O_DIRECT flag

You need to edit the lines in the script above the comment "NO EDITABLE PARAMETERS BELOW THIS LINE".  The parameters are:

- threads - how many threads per client
- files - files per thread to access
- filesize_kb - how much data to read/write to each file in KB
- recordsize_kb - how much data to transfer in a single read() or write() call
- clientFile - name of file containing the list of clients (1 client per line)
- GFAPI_VOLNAME - Gluster volume name
- GFAPI_HOSTNAME - Gluster server hostname of a server that participates in the volume
- GFAPI_LOAD - workload type (seq-wr, seq-rd, rnd-wr, rnd-rd) - for small files typically only seq-wr or seq-rd are used
-- seq-wr = sequential write
-- seq-rd = sequential read
-- rnd-wr = random write
-- rnd-rd = random read
- MOUNTPOINT - the script needs a Gluster volume mountpoint to do basic administrative and setup tasks for the test
- TOPDIR - directory to use within the mountpoint.   It is a relative path, but prefix it with a "/" - libgfapi considers it an absolute path (within that volume)

Here's a sample run:

[root@gprfc032-10ge gfapi]# PSMF_TOPDIR=/wrt PSMF_FILES=500 PSMF_LOAD=seq-rd bash parallel_glfs_smf.sh
volume name: ALU
Gluster server in the volume: gprfs034-10ge
workload: seq-rd
list of clients in file: clients.list
record size (KB): 1024
file size (KB): 1024
files per thread: 500
threads per client: 8
test driver glusterfs mountpoint: /mnt/jbod-ALU
top directory within Gluster volume: /wrt
each thread (process) runs program at: /usr/local/bin/glfs_smf
log files for each libgfapi process at /tmp/glfs_smf_logs
Mon Mar 24 14:55:06 EDT 2014: starting clients ... gprfc077 gprfc078 gprfc080 gprfc081 gprfc082 gprfc083 gprfc084 gprfc085
Mon Mar 24 14:55:09 EDT 2014: clients are all ready
Mon Mar 24 14:55:09 EDT 2014 : clients should all start running within a few seconds
Mon Mar 24 14:55:25 EDT 2014: clients completed
 per-thread results in /var/tmp/glfs_smf_all_logs/result.csv
 transfer-rate: 2394.49 MBytes/s
 file-rate:  2394.49 files/sec
 IOPS: 2394.49 requests/sec
