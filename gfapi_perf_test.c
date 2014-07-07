/*
 * gfapi_perf_test.c - single-thread test of Gluster libgfapi file perf, enhanced to do small files also
 *
 * install the glusterfs-api RPM before trying to compile and link
 *
 * to compile: gcc -pthreads -g -O0  -Wall --pedantic -o gfapi_perf_test -I /usr/include/glusterfs/api gfapi_perf_test.c  -lgfapi -lrt
 *
 * environment variables used as inputs, see usage() below
 *
 * NOTE: we allow random workloads to process a fraction of the entire file
 * this allows us to generate a file that will not fit in cache 
 * we then can do random I/O on a fraction of the data in that file, unlike iozone
 */

#define _GNU_SOURCE
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <memory.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include <fcntl.h>
#include "glfs.h"

#define NOTOK 1 /* process exit status indicates error of some sort */
#define OK 0    /* system call or process exit status indicating success */
#define KB_PER_MB 1024
#define BYTES_PER_KB 1024
#define BYTES_PER_MB (1024*1024)
#define KB_PER_MB 1024
#define NSEC_PER_SEC 1000000000.0
#define UINT64DFMT "%ld"

/* power of 2 corresponding to 4096-byte page boundary, used in memalign() call */
#define PAGE_BOUNDARY 12 

#define FOREACH(_index, _count) for(_index=0; _index < (_count); _index++)

/* last array element of workload_types must be NULL */
static const char * workload_types[] = 
   { "seq-wr", "seq-rd", "rnd-wr", "rnd-rd", "unlink", "seq-rdwrmix", NULL };
static const char * workload_description[] = 
   { "sequential write", "sequential read", "random write", "random read", "delete", "sequential read-write mix", NULL };
/* define numeric workload types as indexes into preceding array */
#define WL_SEQWR 0
#define WL_SEQRD 1
#define WL_RNDWR 2
#define WL_RNDRD 3
#define WL_DELETE 4
#define WL_SEQRDWRMIX 5

static glfs_t * glfs_p = NULL;

/* shared parameter values common to all threads */

struct gfapi_prm {
  int threads_per_proc;            /* threads spawned within each process */
  char * workload_str;             /* name of workload to run */
  int workload_type;               /* post-parse numeric code for workload - contains WL_something */
  unsigned usec_delay_per_file;    /* microseconds of delay between each file operation */
  int recsz;                       /* I/O transfer size (KB) */
  uint64_t filesz_kb;              /* file size (KB) */
  int filecount;                   /* how many files per thread */
  uint64_t io_requests;            /* if random I/O, how many I/O requests to issue per thread */
  int files_per_dir;               /* max files placed in each subdirectory beneath thread directory */
  float rdpct;                     /* read percentage for mixed workload */
  char * prefix;                   /* filename prefix (lets you run multiple creates in same directory) */
  char * thrd_basedir;             /* per-thread base directory */
  char * starting_gun_file;        /* name of file that tells threads to start running */
  int fsync_at_close;              /* on write tests, whether or not to call fsync() before close() */
  int use_fuse;                    /* if TRUE, use POSIX filesystem calls, otherwise use libgfapi. default libgfapi */
  int o_direct;                    /* use O_DIRECT flag? */
  int o_append;                    /* use O_APPEND flag? */
  int o_overwrite;                 /* overwrite the file instead of creating it? */
  unsigned bytes_to_xfer;          /* io size in bytes instead of KB */
  int trclvl;                      /* libgfapi tracing level */
  char * glfs_volname;             /* Gluster volume name */
  char * glfs_hostname;            /* Gluster server participating in that volume */
  char * glfs_transport;           /* transport protocol (RDMA or TCP, defaults to TCP) */
  int glfs_portnum;                /* port number (DO WE NEED?) */
  int open_flags;                  /* calculate flags to use with open or glfs_open */
  int starting_gun_timeout;        /* how long should threads wait for starting gun to fire */
  int debug;                       /* debugging messages */
};
static struct gfapi_prm prm = {0};  /* initializer ensures everything is zero (static probably is anyway) */

/* per-thread data structure */

struct gfapi_result {
  pthread_t thr;
  int thread_num;
  uint64_t elapsed_time, end_time, start_time;
  uint64_t total_bytes_xferred, total_io_count;
  uint64_t files_read, files_written, files_deleted;
};
typedef struct gfapi_result gfapi_result_t;



/*** code begins here ****/

char * now_str(void) {
        time_t now = time((time_t * )NULL);
        char * timebuf = (char * )malloc(100);
        if (!timebuf) return timebuf;
        strcpy(timebuf, ctime(&now));
        timebuf[strlen(timebuf)-1] = 0;
        return timebuf; /* MEMORY LEAK, doesn't matter unless you do it a lot */
}

/* if system call error occurs, call this to print errno and then exit */

void scallerr(char * msg)
{
        printf("%s : %s : errno (%d)%s\n", now_str(), msg, errno, strerror(errno));
        if (glfs_p) glfs_fini(glfs_p);
        exit(NOTOK); 
}

/* if user inputs are wrong, print this and exit */

void usage2(const char * msg, const char * param)
{
        if (param) { printf(msg, param); puts(""); }
        else puts(msg);
        puts("usage: ./gfapi_perf_test");
        puts("environment variables may be inserted at front of command or exported");
        puts("defaults are in parentheses");
        puts("DEBUG (0 means off)     - print everything the program does");
        puts("GFAPI_VOLNAME           - Gluster volume to use");
        puts("GFAPI_HOSTNAME          - Gluster server participating in the volume");
        puts("GFAPI_TRANSPORT (tcp)   - transport protocol to use, can be tcp or rdma");
        puts("GFAPI_PORT (24007)      - port number to connect to");
        puts("GFAPI_RECSZ (64)        - I/O transfer size (i.e. record size) to use");
        puts("GFAPI_FSZ (1M)          - file size ");
        puts("GFAPI_BASEDIR(/tmp)     - directory for this thread to use");
        puts("GFAPI_LOAD (seq-wr)     - workload to apply, can be one of seq-wr, seq-rd, rnd-wr, rnd-rd, unlink, seq-rdwrmix");
        puts("GFAPI_IOREQ (0 = all)   - for random workloads , how many requests to issue");
        puts("GFAPI_DIRECT (0 = off)  - force use of O_DIRECT even for sequential reads/writes");
        puts("GFAPI_FUSE (0 = false)  - if true, use POSIX (through FUSE) instead of libgfapi");
        puts("GFAPI_TRCLVL (0 = none) - trace level specified in glfs_set_logging");
        puts("GFAPI_FILES (100)       - number of files to access");
        puts("GFAPI_STARTING_GUN (none) - touch this file to begin test after all processes are started");
        puts("GFAPI_STARTING_GUN_TIMEOUT (60) - each thread waits this many seconds for starting gun file before timing out");
        puts("GFAPI_FILES_PER_DIR (1000) - maximum files placed in a leaf directory");
        puts("GFAPI_APPEND (0)        - if 1, then append to existing file, instead of creating it");
        puts("GFAPI_OVERWRITE (0)     - if 1, then overwrite existing file, instead of creating it");
        puts("GFAPI_PREFIX (none)     - insert string in front of filename");
        puts("GFAPI_USEC_DELAY_PER_FILE (0) - if non-zero, then sleep this many microseconds after each file is accessed");
        puts("GFAPI_FSYNC_AT_CLOSE (0) - if 1, then issue fsync() call on file before closing");
        /* puts("GFAPI_DIRS_PER_DIR (1000) - maximum subdirs placed in a directory"); */
        exit(NOTOK);
}

void usage(const char * msg) { usage2(msg, NULL); }

/* get an integer environment variable, returning default value if undefined */

int getenv_int( const char * env_var, const int default_value)
{
        char * str_val = getenv(env_var);
        int val = default_value;
        if (str_val) val = atoi(str_val);
        /* printf("getenv_int: returning value %d for variable %s\n", val, env_var); */
        return val;
}

/* get a floating-point environment variable, returning default value if undefined */

float getenv_float( const char * env_var, const float default_value)
{
        char * str_val = getenv(env_var);
        float val = default_value;
        if (str_val) val = atof(str_val);
        /* printf("getenv_float: returning value %f for variable %s\n", val, env_var); */
        return val;
}

/* get an integer file size environment variable, returning default value if undefined */

uint64_t getenv_size64_kb( const char * env_var, const uint64_t default_value)
{
        char * str_val = getenv(env_var);
        uint64_t val = default_value;
        int slen;
        if (str_val) {
          slen = strlen(str_val);
          if (slen > 0) {
            char lastch = str_val[slen-1];
            val = atoi(str_val);
            if (isalpha(lastch)) {
              str_val[slen-1] = 0; /* drop the unit */
              switch (toupper(lastch)) {
                case 'M':
                  val *= KB_PER_MB;
                  break;
                case 'K':
                  break;
                case 'G':
                  val *= (KB_PER_MB * KB_PER_MB);
                  break;
                case 'T':
                  val *= (KB_PER_MB * KB_PER_MB * KB_PER_MB);
                  break;
                default:
                  usage("use lower- or upper-case suffixes K, M, G, or T for file size");
              }
            }
          }
        }
        return val;
}

/* get a string environment variable, returning default value if undefined */

char * getenv_str( const char * env_var, const char * default_value)
{
        char * str_val = getenv(env_var);
        const char * val = default_value;
        if (str_val) val = str_val;
        else if (!default_value) 
                usage2("getenv_str: you must define environment variable %s", env_var);
        /* printf("getenv_str: returning value %s for variable %s\n", val, env_var); */
        return (char * )val;
}

/* get current time in nanosec */

uint64_t gettime_ns(void)
{
        uint64_t ns;
        struct timespec t;

        clock_gettime(CLOCK_REALTIME, &t);
        ns = t.tv_nsec + 1000000000*t.tv_sec;
        return ns;
}

void sleep_for_usec( unsigned usec_delay_per_file )
{
     int rc;
     struct timeval tval = {0};
     tval.tv_usec = usec_delay_per_file;
     rc = select( 0, NULL, NULL, NULL, &tval );
     if (rc < OK) scallerr("select");
}

/* used to generate random offsets into a file for random I/O workloads */

off_t * random_offset_sequence( uint64_t file_size_bytes, size_t record_size_bytes )
{
        unsigned j;
        uint64_t io_requests = file_size_bytes / record_size_bytes;
        off_t * offset_sequence = (off_t * )calloc(io_requests, sizeof(off_t));

        for (j=0; j<io_requests; j++) offset_sequence[j] = j*record_size_bytes;
        for (j=0; j<io_requests; j++) {
                off_t next_offset = offset_sequence[j];
                unsigned random_index = random() % io_requests;
                off_t random_offset = offset_sequence[random_index];
                offset_sequence[j] = random_offset;
                offset_sequence[random_index] = next_offset;
        }
        return offset_sequence;
}

/* compute next pathname for thread to use */

void get_next_path( const int filenum, const int files_per_dir, const int thread_num, const char * base_dir, const char * prefix, char *next_fname  )
{
   int subdir = filenum / files_per_dir;
   sprintf(next_fname, "%s/thrd%03d-d%04d/%s.%07d", base_dir, thread_num, subdir, prefix, filenum);
}

/* each thread runs code in this routine */

void * gfapi_thread_run( void * void_result_p )
{
  gfapi_result_t * result_p = (gfapi_result_t * )void_result_p;
  off_t * random_offsets;
  char ready_path[1024] = {0}, hostnamebuf[1024] = {0}, pidstr[100] = {0}, threadstr[100] = {0};
  int ready_fd;
  glfs_fd_t * ready_fd_p;
  glfs_fd_t * glfs_fd_p = NULL;
  int fd = -1;
  int rc, k;
  int sec;
  struct stat st = {0};
  char next_fname[1024] = {0};
  int create_flags = O_WRONLY|O_EXCL|O_CREAT;
  off_t offset;
  unsigned io_count;
  int bytes_xferred;
  char * buf;

  /* use same random offset sequence for all files */

  if (prm.workload_type == WL_RNDWR || prm.workload_type == WL_RNDRD) {
    random_offsets = random_offset_sequence( 
                          (uint64_t )prm.filesz_kb*BYTES_PER_KB, prm.recsz*BYTES_PER_KB );
  }

  /* wait for the starting gun file, which should be in parent directory */
  /* it is invoker's responsibility to unlink the starting gun file before starting this program */

  if (strlen(prm.starting_gun_file) > 0) {
    static const int sg_create_flags = O_CREAT|O_EXCL|O_WRONLY;
    char ready_buf[1024] = {0};

    /* signal that we are ready */

    gethostname(hostnamebuf, sizeof(hostnamebuf)-4);
    sprintf(pidstr, "%d", getpid());
    sprintf(threadstr, "%d", result_p->thread_num);

    strcpy(ready_buf, prm.starting_gun_file);
    dirname(ready_buf);
    strcpy(ready_path, ready_buf);
    strcat(ready_path, "/");
    strcat(ready_path, strtok(hostnamebuf,"."));
    strcat(ready_path, ".");
    strcat(ready_path, pidstr);
    strcat(ready_path, ".");
    strcat(ready_path, threadstr);
    strcat(ready_path, ".ready");
    printf("%s : ", now_str());
    printf("signaling ready with file %s\n", ready_path);
    if (prm.use_fuse) {
      ready_fd = open(ready_path, sg_create_flags, 0666);
      if (ready_fd < 0) scallerr(ready_path);
      else {
        rc = close(ready_fd);
        if (rc < OK) scallerr("ready path close");
      }
    } else {
      ready_fd_p = glfs_creat(glfs_p, ready_path, sg_create_flags, 0644);
      if (!ready_fd_p) scallerr(ready_path);
      else {
        rc = glfs_close(ready_fd_p);
        if (rc < OK) scallerr("ready path close");
      }
    }

    /* wait until we are told to start the test, to give other threads time to get ready */

    printf("%s : ", now_str());
    printf("awaiting starting gun file %s\n", prm.starting_gun_file);
    FOREACH(sec, prm.starting_gun_timeout) {
      rc = prm.use_fuse ? stat(prm.starting_gun_file, &st) : glfs_stat(glfs_p, prm.starting_gun_file, &st);
      if (prm.debug) printf("rc=%d errno=%d\n", rc, errno);
      if (rc != OK) {
        if (errno != ENOENT) scallerr(prm.use_fuse ? "stat" : "glfs_stat");
      } else {
        break; /* we heard the starting gun */
      }
      sleep(1);
    }
    if (sec == prm.starting_gun_timeout) {
      printf(now_str());
      printf("ERROR: timed out after %d sec waiting for starting gun file %s\n", 
             prm.starting_gun_timeout, prm.starting_gun_file);
      exit(NOTOK);
    }
    sleep(3); /* give everyone a chance to see it */
  }

  /* we can use page-aligned buffer regardless of whether O_DIRECT is used or not */
  buf = memalign(PAGE_BOUNDARY, prm.bytes_to_xfer);
  if (!buf) scallerr("posix_memalign");

  /* open the file */

  result_p->start_time = gettime_ns();
  create_flags |= prm.o_direct;
  if (prm.o_append|prm.o_overwrite) create_flags &= ~(O_EXCL|O_CREAT);
  FOREACH(k, prm.filecount) {
   int workload = prm.workload_type;
   if (prm.debug) printf("starting file %s\n", next_fname);
   if (workload == WL_SEQRDWRMIX) {
     float rndsample = (float )(random() % 100);
     workload = (rndsample > prm.rdpct) ? WL_SEQWR : WL_SEQRD;
     if (prm.debug) printf("workload %s\n", workload_description[workload]);
   }
   get_next_path( k, prm.files_per_dir, result_p->thread_num, prm.thrd_basedir, prm.prefix, next_fname );
   fd = -2;
   glfs_fd_p = NULL;
   if (prm.use_fuse) {
     switch (workload) {
      case WL_DELETE:
        rc = unlink(next_fname);
        if (rc < OK && errno != ENOENT) scallerr(next_fname);
        break;

      case WL_SEQWR: 
        fd = open(next_fname, create_flags, 0666);
        if ((fd < OK) && (errno == ENOENT)) {
          char subdir[1024];
          strcpy(subdir, dirname(next_fname));
          rc = mkdir(subdir, 0755);
          if (rc < OK) scallerr(subdir);
          /* we have to reconstruct filename because dirname() function sticks null into it */
          get_next_path( k, prm.files_per_dir, result_p->thread_num, prm.thrd_basedir, prm.prefix, next_fname );
          fd = open(next_fname, create_flags, 0666);
        }
        if ((prm.workload_type == WL_SEQRDWRMIX) && (rc < OK) && (errno == EEXIST)) {
          rc = unlink(next_fname);
          if (rc < OK) scallerr(next_fname);
          fd = open(next_fname, create_flags, 0666);
        }
        if (fd < OK) scallerr(next_fname);
        if (prm.o_append) {
          rc = lseek( fd, 0, SEEK_END);
          if (rc < OK) scallerr(next_fname);
        }
        break;

      case WL_SEQRD:
        fd = open(next_fname, O_RDONLY|prm.o_direct);
        if (fd < OK) scallerr(next_fname);
        break;

      case WL_RNDWR:
        fd = open(next_fname, O_WRONLY|prm.o_direct);
        if (fd < OK) scallerr(next_fname);
        break;

      case WL_RNDRD:
        fd = open(next_fname, O_RDONLY|prm.o_direct);
        if (fd < OK) scallerr(next_fname);
        break;

      default: exit(NOTOK);
     }
   } else {
     switch (workload) {
      case WL_DELETE:
        rc = glfs_unlink(glfs_p, next_fname);
        if (rc < OK && errno != ENOENT) scallerr(next_fname);
        break;

      case WL_SEQWR: 
        if (prm.o_append|prm.o_overwrite) {
          glfs_fd_p = glfs_open(glfs_p, next_fname, create_flags );
          if (!glfs_fd_p) scallerr(next_fname);
          if (prm.o_append) {
            rc = glfs_lseek( glfs_fd_p, 0, SEEK_END);
            if (rc < OK) scallerr(next_fname);
          }
        } else {
          glfs_fd_p = glfs_creat(glfs_p, next_fname, create_flags, 0666 );
          if ((!glfs_fd_p) && (errno == ENOENT)) {
            char subdir[1024];
            strcpy(subdir, dirname(next_fname));
            rc = glfs_mkdir(glfs_p, subdir, 0755);
            if (rc < OK) scallerr(subdir);
            /* we have to reconstruct filename because dirname() function sticks null into it */
            get_next_path( k, prm.files_per_dir, result_p->thread_num, prm.thrd_basedir, prm.prefix, next_fname );
            glfs_fd_p = glfs_creat(glfs_p, next_fname, create_flags, 0666);
          }
          if ((prm.workload_type == WL_SEQRDWRMIX) && (rc < OK) && (errno == EEXIST)) {
            rc = glfs_unlink(glfs_p, next_fname);
            if (rc < OK && errno != ENOENT) scallerr(next_fname);
            glfs_fd_p = glfs_creat(glfs_p, next_fname, create_flags, 0666);
          }
          if (!glfs_fd_p) scallerr(next_fname);
        }
        break;

      case WL_SEQRD:
        glfs_fd_p = glfs_open(glfs_p, next_fname, O_RDONLY|prm.o_direct);
        if (!glfs_fd_p) scallerr(next_fname);
        break;

      case WL_RNDWR:
        glfs_fd_p = glfs_open(glfs_p, next_fname, O_WRONLY|prm.o_direct);
        if (!glfs_fd_p) scallerr(next_fname);
        break;

      case WL_RNDRD:
        glfs_fd_p = glfs_open(glfs_p, next_fname, O_RDONLY|prm.o_direct);
        if (!glfs_fd_p) scallerr(next_fname);
        break;

      default: exit(NOTOK);
     }
   }
   if (workload == WL_DELETE) {
     if (prm.usec_delay_per_file) sleep_for_usec(prm.usec_delay_per_file);
     result_p->files_deleted++;
     continue;
   }

   /* perform the requested I/O operations */

   offset = 0;
   if (prm.debug) printf("io_requests = %ld\n", prm.io_requests);
   FOREACH( io_count, prm.io_requests ) {
    if (workload == WL_SEQWR) {
      offset += prm.bytes_to_xfer;
      bytes_xferred = prm.use_fuse ?
        write(fd, buf, prm.bytes_to_xfer) :
        glfs_write(glfs_fd_p, buf, prm.bytes_to_xfer, 0);
      if (bytes_xferred < prm.bytes_to_xfer) 
                scallerr(prm.use_fuse?"write":"glfs_write");

    } else if (workload == WL_SEQRD) {
      offset += prm.bytes_to_xfer;
      bytes_xferred = prm.use_fuse ? 
        read(fd, buf, prm.bytes_to_xfer) :
        glfs_read(glfs_fd_p, buf, prm.bytes_to_xfer, 0);
      if (bytes_xferred < prm.bytes_to_xfer) 
                scallerr(prm.use_fuse?"read":"glfs_read");

    } else if (workload == WL_RNDWR) {
      offset = random_offsets[io_count];
      bytes_xferred = prm.use_fuse ?
        pwrite(fd, buf, prm.bytes_to_xfer, offset) :
        glfs_pwrite(glfs_fd_p, buf, prm.bytes_to_xfer, offset, 0);
      if (bytes_xferred < prm.bytes_to_xfer) 
                scallerr(prm.use_fuse?"pwrite":"glfs_pwrite");

    } else if (workload == WL_RNDRD) {
      offset = random_offsets[io_count];
      bytes_xferred = prm.use_fuse ? 
        pread(fd, buf, prm.bytes_to_xfer, offset) :
        glfs_pread(glfs_fd_p, buf, prm.bytes_to_xfer, offset, 0);
      if (bytes_xferred < prm.bytes_to_xfer) 
                scallerr(prm.use_fuse?"pwrite":"glfs_pwrite");
    }
    result_p->total_bytes_xferred += bytes_xferred;
    if (prm.debug) printf("offset %-20ld, io_count %-10u total_bytes_xferred %-20ld\n", 
                      offset, io_count, result_p->total_bytes_xferred);
   }
   result_p->total_io_count += io_count;

   /* shut down file access */

   if ((workload == WL_SEQWR || workload == WL_RNDWR) && prm.fsync_at_close) {
     rc = prm.use_fuse ? fsync(fd) : glfs_fsync(glfs_fd_p);
     if (rc) scallerr(prm.use_fuse ? "fsync" : "glfs_fsync");
   }
   rc = prm.use_fuse ? close(fd) : glfs_close(glfs_fd_p);
   if (rc) scallerr(prm.use_fuse ? "close" : "glfs_close");
   if (prm.usec_delay_per_file) sleep_for_usec(prm.usec_delay_per_file);
   if ((workload == WL_SEQWR) || (workload == WL_RNDWR))
     result_p->files_written++;
   if ((workload == WL_SEQRD) || (workload == WL_RNDRD))
     result_p->files_read++;
  }
  result_p->end_time = gettime_ns();
  return NULL;
}

void print_result( gfapi_result_t * result_p )
{
  float thru, files_thru, mb_transferred, pct_actual_reads;
  uint64_t files_done;

  /* calculate and print stats */

  if (result_p->thread_num < 0) printf("aggregate: "); else printf("thread %3d: ", result_p->thread_num);
  result_p->elapsed_time = result_p->end_time - result_p->start_time;
  if (prm.debug) printf("start %ld end %ld elapsed %ld\n", result_p->start_time, result_p->end_time, result_p->elapsed_time);
  if (prm.debug) printf("  total byte count = "UINT64DFMT" total io count = "UINT64DFMT"\n", 
                     result_p->total_bytes_xferred, result_p->total_io_count );
  mb_transferred = (float )result_p->total_io_count * prm.recsz / KB_PER_MB;
  thru = mb_transferred * NSEC_PER_SEC / result_p->elapsed_time ;
  files_done = result_p->files_written + result_p->files_read;
  files_thru = files_done * NSEC_PER_SEC / result_p->elapsed_time;
  if (files_done < 10) {
    files_thru = 0.0;
  }
  if (result_p->files_written) printf("  files written = "UINT64DFMT"\n", result_p->files_written);
  if (result_p->files_read) printf("  files read = "UINT64DFMT"\n", result_p->files_read);
  printf("  files done = "UINT64DFMT"\n", files_done);
  if (prm.workload_type == WL_SEQRDWRMIX) {
    pct_actual_reads = 100.0 * result_p->files_read / files_done;
    printf("  fraction of reads = %6.2f%%\n", pct_actual_reads );
  }
  if (result_p->total_io_count > 0) printf("  I/O (record) transfers = "UINT64DFMT"\n", result_p->total_io_count);
  if (result_p->total_bytes_xferred > 0) printf("  total bytes = "UINT64DFMT"\n", result_p->total_bytes_xferred);
  printf("  elapsed time    = %-9.2f sec\n", result_p->elapsed_time/NSEC_PER_SEC);
  if (thru > 0.0) printf("  throughput      = %-9.2f MB/sec\n", thru);
  if (files_thru > 0.0) printf("  file rate       = %-9.2f files/sec\n", files_thru);
  if (thru > 0.0) printf("  IOPS            = %-9.2f (%s)\n", thru * 1024 / prm.recsz, workload_description[prm.workload_type]);
}

void aggregate_result( gfapi_result_t * r_in_p, gfapi_result_t * r_out_p )
{
  if (r_out_p->start_time == 0) r_out_p->start_time = (uint64_t )-1; /* positive infinity */
  if (r_out_p->start_time > r_in_p->start_time) r_out_p->start_time = r_in_p->start_time;
  if (r_out_p->end_time < r_in_p->end_time) r_out_p->end_time = r_in_p->end_time;
  r_out_p->total_bytes_xferred += r_in_p->total_bytes_xferred;
  r_out_p->total_io_count += r_in_p->total_io_count;
  r_out_p->files_read += r_in_p->files_read;
  r_out_p->files_written += r_in_p->files_written;
  r_out_p->files_deleted += r_in_p->files_deleted;
}

int main(int argc, char * argv[])
{
  int rc, j, t;
  uint64_t max_io_requests;
  gfapi_result_t * result_array;
  gfapi_result_t aggregate = {0};

  /* define environment variable inputs */

  prm.debug = getenv_int("DEBUG", 0);
  prm.rdpct = getenv_float("GFAPI_RDPCT", 0.0);
  prm.threads_per_proc = getenv_int("GFAPI_THREADS_PER_PROC", 1);
  prm.trclvl = getenv_int("GFAPI_TRCLVL", 0);
  prm.glfs_volname = getenv_str("GFAPI_VOLNAME", NULL);
  prm.glfs_hostname = getenv_str("GFAPI_HOSTNAME", NULL);
  prm.glfs_transport = getenv_str("GFAPI_TRANSPORT", "tcp");
  prm.glfs_portnum = getenv_int("GFAPI_PORT", 24007);
  prm.recsz = getenv_int("GFAPI_RECSZ", 64);
  prm.filesz_kb = getenv_size64_kb("GFAPI_FSZ", 1024);
  prm.prefix = getenv_str("GFAPI_PREFIX", "f");
  prm.thrd_basedir = getenv_str("GFAPI_BASEDIR", "/tmp" );
  prm.starting_gun_file = getenv_str("GFAPI_STARTING_GUN", "");
  prm.workload_str = getenv_str("GFAPI_LOAD", "seq-wr");  
  prm.io_requests = (uint64_t )getenv_int("GFAPI_IOREQ", 0);
  prm.starting_gun_timeout = getenv_int("GFAPI_STARTING_GUN_TIMEOUT", 60);
  prm.fsync_at_close = getenv_int("GFAPI_FSYNC_AT_CLOSE", 0);
  prm.use_fuse = getenv_int("GFAPI_FUSE", 0);
  prm.o_direct = getenv_int("GFAPI_DIRECT", 0) ? O_DIRECT : 0;
  prm.o_append = getenv_int("GFAPI_APPEND", 0);
  prm.o_overwrite = getenv_int("GFAPI_OVERWRITE", 0);
  prm.filecount = getenv_int("GFAPI_FILES", 100);
  prm.usec_delay_per_file = getenv_int("GFAPI_USEC_DELAY_PER_FILE", 0);
  /* int dirs_per_dir = getenv_int("GFAPI_DIRS_PER_DIR", 1000); */
  prm.files_per_dir = getenv_int("GFAPI_FILES_PER_DIR", 1000);

  printf("GLUSTER: \n  volume=%s\n  transport=%s\n  host=%s\n  port=%d\n  fuse?%s\n  trace level=%d\n  start timeout=%d\n", 
                prm.glfs_volname, prm.glfs_transport, prm.glfs_hostname, prm.glfs_portnum, prm.use_fuse ? "Yes" : "No", prm.trclvl, prm.starting_gun_timeout );
  printf("WORKLOAD:\n  type = %s \n  threads/proc = %d\n  base directory = %s\n  prefix=%s\n"
         "  file size = "UINT64DFMT" KB\n  file count = %d\n  record size = %u KB"
         "\n  files/dir=%d\n  fsync-at-close? %s \n", 
                prm.workload_str, prm.threads_per_proc, prm.thrd_basedir, prm.prefix, 
                prm.filesz_kb, prm.filecount, prm.recsz, 
                prm.files_per_dir, prm.fsync_at_close?"Yes":"No");
  if (prm.o_direct) printf("  forcing use of direct I/O with O_DIRECT flag in open call\n");
  if (prm.usec_delay_per_file) printf("  sleeping %d microsec after each file access\n", prm.usec_delay_per_file);
  if (argc > 1) usage("glfs_io_test doesn't take command line parameters");
  if (prm.o_append && prm.o_overwrite) usage("GFAPI_APPEND and GFAPI_OVERWRITE cannot be used in the same test");

  /* validate inputs */

  for (j=0; workload_types[j]; j++) {
    if (strcmp(workload_types[j], prm.workload_str) == 0)
        break;
  }
  if (!workload_types[j]) usage2("invalid workload type %s", prm.workload_str);
  prm.workload_type = j; /* one of WL_* codes */
  if (prm.workload_type == WL_SEQRDWRMIX) {
    printf( "  percent reads = %6.2f\n", prm.rdpct );
    if ((prm.o_append == 0) && (prm.o_overwrite == 0)) prm.o_append = 1;
  }
  if (prm.o_append) printf("  using O_APPEND flag to append to existing files\n");
  if (prm.o_overwrite) printf("  overwriting existing files\n");

  if (prm.filesz_kb < prm.recsz) {
    printf("  truncating record size %u KB to file size %lu KB\n", prm.recsz, prm.filesz_kb );
    prm.recsz = prm.filesz_kb;
  }
  max_io_requests = prm.filesz_kb / prm.recsz;
  if (prm.workload_type == WL_RNDRD || prm.workload_type == WL_RNDWR) {
    if (prm.io_requests == 0) prm.io_requests = max_io_requests;
    printf("  random read/write requests = "UINT64DFMT"\n", prm.io_requests);
    if (prm.io_requests > max_io_requests) {
        usage("GFAPI_IOREQ too large for file size and record size");
    }
  } else { /* if sequential workload, do entire file  */
    prm.io_requests = max_io_requests;
  }
  if (prm.debug) printf("max_io_requests = %ld\n", (long )max_io_requests);

  srandom(time(NULL));
  prm.bytes_to_xfer = prm.recsz * BYTES_PER_KB;

  /* initialize libgfapi instance */

  if (!prm.use_fuse) {
    char logfilename[100];
    /* mount volume */
    glfs_p = glfs_new(prm.glfs_volname);
    if (!glfs_p) scallerr("ERROR: could not initialize Gluster volume mount with volname");

    sprintf(logfilename, "/tmp/glfs-%d.log", getpid());
    if (glfs_set_logging(glfs_p, logfilename, prm.trclvl)) scallerr("set_logging");
  
    if (glfs_set_volfile_server( glfs_p, prm.glfs_transport, prm.glfs_hostname, prm.glfs_portnum ))
        scallerr("ERROR: could not initialize gfapi mount");

    rc = glfs_init(glfs_p);
    if (rc) scallerr("glfs_init");
  }

  /* allocate and initialize per-thread structure and start each thread */

  result_array = (gfapi_result_t * )calloc(prm.threads_per_proc, sizeof(gfapi_result_t));
  FOREACH(t, prm.threads_per_proc) {
    gfapi_result_t * next_result_p = &result_array[t];
    next_result_p->thread_num = t;
    rc = pthread_create(&next_result_p->thr, NULL, gfapi_thread_run, next_result_p);
    if (rc != OK) scallerr("pthread_create");
  }
   
  /* wait for each thread to finish */

  FOREACH(t, prm.threads_per_proc) {
    void * retval;
    gfapi_result_t * next_result_p = &result_array[t];
    rc = pthread_join( next_result_p->thr, &retval );
    if (rc != OK) {
      printf("thread %d return code %d\n", t, rc);
    }
    if (retval == PTHREAD_CANCELED) {
      printf("thread %d cancelled\n", t);
    }
    if (retval) {
      printf("thread %d failed with rc %p\n", t, retval);
    }
  }
  if (!prm.use_fuse) {
    rc = glfs_fini(glfs_p);
    if (rc < OK) scallerr("glfs_fini");
  }
  FOREACH(t, prm.threads_per_proc) {
    print_result(&result_array[t]);
    aggregate_result(&result_array[t], &aggregate);
  }
  aggregate.thread_num = -1;
  print_result(&aggregate);
  return OK;
}
