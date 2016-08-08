parallel-libgfapi
=================

benchmark for distributed, multi-thread test of Gluster libgfapi performance

This page describes a new benchmark for measuring small-file performance with libgfapi.  As libgfapi is increasingly used for Gluster applications, questions have arisen about metadata-intensive workload performance with libgfapi and how this scales with bricks, libgfapi instances, etc.   This benchmark takes advantage of the similarity between libgfapi and POSIX interfaces so that the same tests can be run either with libgfapi or in a POSIX filesystem (example: Gluster FUSE mountpoint).  This makes it easy to compare libgfapi performance with FUSE or XFS performance, for example.

This may be the best way to get RDMA to perform well with Gluster as well.  RDMA is based on avoiding data copies between the application and the kernel, but the FUSE (Filesystem in User SpacE) implementation of glusterfs forces data copies between the application and the glusterfs FUSE mountpoint process.  By using libgfapi, an application can bypass these copies.

* low-level test program

The "C" program that runs in each process is gfapi_perf_test.c .  You can run this program by itself.  The command line syntax is a little odd because it uses environment variables.   This is less user-friendly but also easier in some ways.   For example, you don't have to keep entering the same parameters.  To compile, see the command at the top of the program.

To print out environment variables that it supports:

    # ./gfapi_perf_test -h
    usage: ./gfapi_perf_test
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
    GFAPI_LOAD (seq-wr)     - workload to apply, can be one of seq-wr, seq-rd, rnd-wr, rnd-rd, unlink, seq-rdwrmix
    GFAPI_IOREQ (0 = all)   - for random workloads , how many requests to issue
    GFAPI_DIRECT (0 = off)  - force use of O_DIRECT even for sequential reads/writes
    GFAPI_FUSE (0 = false)  - if true, use POSIX (through FUSE) instead of libgfapi
    GFAPI_TRCLVL (0 = none) - trace level specified in glfs_set_logging
    GFAPI_FILES (100)       - number of files to access
    GFAPI_STARTING_GUN (none) - touch this file to begin test after all processes are started
    GFAPI_STARTING_GUN_TIMEOUT (60) - each thread waits this many seconds for starting gun file before timing out
    GFAPI_FILES_PER_DIR (1000) - maximum files placed in a leaf directory
    GFAPI_APPEND (0)        - if 1, then append to existing file, instead of creating it
    GFAPI_OVERWRITE (0)     - if 1, then overwrite existing file, instead of creating it
    GFAPI_PREFIX (none)     - insert string in front of filename
    GFAPI_USEC_DELAY_PER_FILE (0) - if non-zero, then sleep this many microseconds after each file is accessed
    GFAPI_FSYNC_AT_CLOSE (0) - if 1, then issue fsync() call on file before closing

To run a short test on the subdirectory "mytmpdir" within a Gluster volume "demo" served by host gprfs024-10ge:

    # GFAPI_VOLNAME=demo GFAPI_HOSTNAME=gprfs024-10ge GFAPI_BASEDIR=/mytmpdir ./gfapi_perf_test

In this program, it creates subdirectories and puts no more than GFAPI_FILES_PER_DIR files in each subdirectory.   This allows you to create more files per thread.  

FIXME: It needs an extra level of directories to run really long tests.

* parallel multi-client test script

The parallel_gfapi_test.sh script launches a multi-threaded, distributed test using the above program.  Someday it may switch to using fio with the libgfapi engine developed by Huamin Chen, but for now it's simpler to do it this way.  Environment variables supported by this script are in comments at top of the script. You may need to edit a few the lines in the script above the comment NO EDITABLE PARAMETERS BELOW THIS LINE.  

Here's a sample run:

    # env | grep PGFAPI
    PGFAPI_PROGRAM=/root/parallel-libgfapi/gfapi_perf_test
    # PGFAPI_MOUNTPOINT=/mnt/test PGFAPI_PROCESSES=1 bash ./parallel_gfapi_test.sh
    volume name: test2
    Gluster server in the volume: 172.17.50.86
    workload: seq-wr
    list of clients in file: clients.list
    record size (KB): 64
    file size (KB): 4
    files per thread: 10240
    processes per client: 1
    threads per process: 1
    test driver glusterfs mountpoint: /mnt/test
    top directory within Gluster volume: /gfapi-test
    each thread (process) runs program at: /root/parallel-libgfapi/gfapi_perf_test
    log files for each libgfapi process at /tmp/parallel_gfapi_logs.12266
    starting gun timeout = 10
    removing any previous files
    Thu Sep 25 11:16:24 EDT 2014: starting 1 clients ... perf88 
    ls: cannot access /mnt/test//gfapi-test/*.ready: No such file or directory
    ...
    Thu Sep 25 11:16:30 EDT 2014: clients are all ready
    Thu Sep 25 11:16:30 EDT 2014 : clients should all start running within a few seconds
    per-thread results in /tmp/parallel_gfapi_logs.12266/result.csv
    transfer-rate: 1.0 MBytes/s
    file-rate:  1.0 files/sec
    IOPS: IOPS: 1.0 requests/sec
    


