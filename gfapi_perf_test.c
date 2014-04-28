/*
 * gfapi_perf_test.c - single-thread test of Gluster libgfapi file perf, enhanced to do small files also
 *
 * install the glusterfs-api RPM before trying to compile and link
 *
 * to compile: gcc -g -O0  -Wall --pedantic -o gfapi_perf_test -I /usr/include/glusterfs/api gfapi_perf_test.c  -lgfapi -lrt
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
#include <sys/types.h>
#include <sys/stat.h>
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

static glfs_t * glfs_p = NULL;

/* if system call error occurs, call this to print errno and then exit */

void scallerr(char * msg)
{
	printf("%s : errno (%d)%s\n", msg, errno, strerror(errno));
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
        puts("GFAPI_LOAD (seq-wr)     - workload to apply, can be one of seq-wr, seq-rd, rnd-wr, rnd-rd, unlink");
        puts("GFAPI_IOREQ (0 = all)   - for random workloads , how many requests to issue");
        puts("GFAPI_DIRECT (0 = off)  - force use of O_DIRECT even for sequential reads/writes");
	puts("GFAPI_FUSE (0 = false)  - if true, use POSIX (through FUSE) instead of libgfapi");
	puts("GFAPI_TRCLVL (0 = none) - trace level specified in glfs_set_logging");
        puts("GFAPI_FILES (100)       - number of files to access");
        puts("GFAPI_STARTING_GUN (none) - touch this file to begin test after all processes are started");
        puts("GFAPI_STARTING_GUN_TIMEOUT (60) - each thread waits this many seconds for starting gun file before timing out");
        puts("GFAPI_FILES_PER_DIR (1000) - maximum files placed in a leaf directory");
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

void get_next_path( int filenum, int files_per_dir, char * base_dir, char *next_fname  )
{
   int subdir = filenum / files_per_dir;
   sprintf(next_fname, "%s/d%04d/f.%07d", base_dir, subdir, filenum);
}

int main(int argc, char * argv[])
{
  static const int create_flags = O_CREAT|O_EXCL|O_WRONLY;
  char next_fname[1024] = {0};
  char ready_path[1024] = {0}, hostnamebuf[1024] = {0}, pidstr[100] = {0};
  glfs_fd_t * glfs_fd_p = NULL;
  struct stat st = {0};
  int fd = -1;
  off_t offset;
  unsigned io_count;
  off_t * random_offsets;
  char * buf;
  int rc, j, k, bytes_to_xfer, bytes_xferred;
  uint64_t start_time, end_time, elapsed_time;
  uint64_t files_done = 0, total_io_count = 0, total_bytes_xferred = 0;
  float thru, files_thru, mb_transferred;
  const char * workload_types[] = { "seq-wr", "seq-rd", "rnd-wr", "rnd-rd", NULL };
#define WL_SEQWR 0
#define WL_SEQRD 1
#define WL_RNDWR 2
#define WL_RNDRD 3
  const char * workload_description[] = { "sequential write", "sequential read", "random write", "random read", NULL };
  int workload_type; 
  uint64_t max_io_requests;
  int ready_fd;
  int sec;
  glfs_fd_t * ready_fd_p;

  /* define environment variable inputs */

  int debug = getenv_int("DEBUG", 0);
  int trclvl = getenv_int("GFAPI_TRCLVL", 0);
  char * glfs_volname = getenv_str("GFAPI_VOLNAME", NULL);
  char * glfs_hostname = getenv_str("GFAPI_HOSTNAME", NULL);
  char * glfs_transport = getenv_str("GFAPI_TRANSPORT", "tcp");
  int glfs_portnum = getenv_int("GFAPI_PORT", 24007);
  int recsz = getenv_int("GFAPI_RECSZ", 64);
  uint64_t filesz_kb = getenv_size64_kb("GFAPI_FSZ", 1024);
  char * thrd_basedir = getenv_str("GFAPI_BASEDIR", "/tmp" );
  char * starting_gun_file = getenv_str("GFAPI_STARTING_GUN", "");
  char * workload_str = getenv_str("GFAPI_LOAD", "seq-wr");  
  uint64_t io_requests = (uint64_t )getenv_int("GFAPI_IOREQ", 0);
  int starting_gun_timeout = getenv_int("GFAPI_STARTING_GUN_TIMEOUT", 60);
  int fsync_at_close = getenv_int("GFAPI_FSYNC_AT_CLOSE", 0);
  int use_fuse = getenv_int("GFAPI_FUSE", 0);
  int o_direct = getenv_int("GFAPI_DIRECT", 0) ? O_DIRECT : 0;
  int filecount = getenv_int("GFAPI_FILES", 100);
  /* int dirs_per_dir = getenv_int("GFAPI_DIRS_PER_DIR", 1000); */
  int files_per_dir = getenv_int("GFAPI_FILES_PER_DIR", 1000);
  printf("GLUSTER: \n  volume=%s\n  transport=%s\n  host=%s\n  port=%d\n  fuse?%s\n  trace level=%d\n  start timeout=%d\n", 
                glfs_volname, glfs_transport, glfs_hostname, glfs_portnum, use_fuse ? "Yes" : "No", trclvl, starting_gun_timeout );
  printf("WORKLOAD:\n  type = %s \n  base directory = %s\n  file size = "UINT64DFMT" KB\n  file count = %d\n  record size = %u KB"
         "\n  files/dir=%d\n  fsync-at-close? %s \n", 
                workload_str, thrd_basedir, filesz_kb, filecount, recsz, files_per_dir, fsync_at_close?"Yes":"No");
  if (o_direct) printf("  forcing use of direct I/O with O_DIRECT flag in open call\n");
  if (argc > 1) usage("glfs_io_test doesn't take command line parameters");

  /* validate inputs */

  for (j=0; workload_types[j]; j++) {
    if (strcmp(workload_types[j], workload_str) == 0)
        break;
  }
  if (!workload_types[j]) usage2("invalid workload type %s", workload_str);
  workload_type = j;

  max_io_requests = filesz_kb / recsz;
  if (workload_type == WL_RNDRD || workload_type == WL_RNDWR) {
    if (io_requests == 0) io_requests = max_io_requests;
    printf("        random read/write requests = "UINT64DFMT"\n", io_requests);
    if (io_requests > max_io_requests) {
        usage("GFAPI_IOREQ too large for file size and record size");
    }
  } else { /* if sequential workload, do entire file  */
    io_requests = max_io_requests;
  }
  if (debug) printf("max_io_requests = %ld\n", (long )max_io_requests);

  /* we can use page-aligned buffer regardless of whether O_DIRECT is used or not */
  bytes_to_xfer = recsz * BYTES_PER_KB;
  buf = memalign(PAGE_BOUNDARY, bytes_to_xfer);
  if (!buf) scallerr("posix_memalign");

  if (!use_fuse) {
    /* mount volume */
    glfs_p = glfs_new(glfs_volname);
    if (!glfs_p) scallerr("ERROR: could not initialize Gluster volume mount with volname");

    if (glfs_set_logging(glfs_p, "glfs.log", trclvl)) scallerr("set_logging");
  
    if (glfs_set_volfile_server( glfs_p, glfs_transport, glfs_hostname, glfs_portnum ))
        scallerr("ERROR: could not initialize gfapi mount");

    rc = glfs_init(glfs_p);
    if (rc) scallerr("glfs_init");
  }

  /* use same random offset sequence for all files */

  if (workload_type == WL_RNDWR || workload_type == WL_RNDRD)
	random_offsets = random_offset_sequence( 
                          (uint64_t )filesz_kb*BYTES_PER_KB, recsz*BYTES_PER_KB );

  /* wait for the starting gun file, which should be in parent directory */
  /* it is invoker's responsibility to unlink the starting gun file before starting this program */

  if (strlen(starting_gun_file) > 0) {
    char ready_buf[1024] = {0};

    /* signal that we are ready */

    gethostname(hostnamebuf, sizeof(hostnamebuf)-4);
    sprintf(pidstr, "%d", getpid());

    strcpy(ready_buf, starting_gun_file);
    dirname(ready_buf);
    strcpy(ready_path, ready_buf);
    strcat(ready_path, "/");
    strcat(ready_path, strtok(hostnamebuf,"."));
    strcat(ready_path, ".");
    strcat(ready_path, pidstr);
    strcat(ready_path, ".ready");
    printf("signaling ready with file %s\n", ready_path);
    if (use_fuse) {
      ready_fd = open(ready_path, create_flags, 0666);
      if (ready_fd < 0) scallerr(ready_path);
      close(ready_fd);
    } else {
      ready_fd_p = glfs_creat(glfs_p, ready_path, create_flags, 0644);
      if (!ready_fd_p) scallerr(ready_path);
      glfs_close(ready_fd_p);
    }

    /* wait until we are told to start the test, to give other threads time to get ready */

    printf("awaiting starting gun file %s\n", starting_gun_file);
    FOREACH(sec, starting_gun_timeout) {
      rc = use_fuse ? stat(starting_gun_file, &st) : glfs_stat(glfs_p, starting_gun_file, &st);
      if (debug) printf("rc=%d errno=%d\n", rc, errno);
      if (rc != OK) {
        if (errno != ENOENT) scallerr(use_fuse ? "stat" : "glfs_stat");
      } else {
        break; /* we heard the starting gun */
      }
      sleep(1);
    }
    if (sec == starting_gun_timeout) {
      printf("ERROR: timed out after %d sec waiting for starting gun file %s\n", 
             starting_gun_timeout, starting_gun_file);
      exit(NOTOK);
    }
    sleep(3); /* give everyone a chance to see it */
  }

  /* open the file */

  start_time = gettime_ns();
  FOREACH(k, filecount) {
   get_next_path( k, files_per_dir, thrd_basedir, next_fname );
   if (debug) printf("starting file %s\n", next_fname);
   fd = -2;
   glfs_fd_p = NULL;
   if (use_fuse) {
     switch (workload_type) {
      case WL_SEQWR: 
        fd = open(next_fname, O_CREAT|O_EXCL|O_WRONLY|o_direct, 0666);
        if ((fd < OK) && (errno == ENOENT)) {
          char subdir[1024];
          strcpy(subdir, dirname(next_fname));
          rc = mkdir(subdir, 0755);
          if (rc < OK) scallerr(subdir);
          get_next_path( k, files_per_dir, thrd_basedir, next_fname );
          fd = open(next_fname, O_CREAT|O_EXCL|O_WRONLY|o_direct, 0666);
        }
        if (fd < OK) scallerr(next_fname);
	break;

      case WL_SEQRD:
        fd = open(next_fname, O_RDONLY|o_direct);
        if (fd < OK) scallerr(next_fname);
	break;

      case WL_RNDWR:
	fd = open(next_fname, O_WRONLY|o_direct);
	if (fd < OK) scallerr(next_fname);
	break;

      case WL_RNDRD:
	fd = open(next_fname, O_RDONLY|o_direct);
	if (fd < OK) scallerr(next_fname);
	break;

      default: exit(NOTOK);
     }
   } else {
     switch (workload_type) {
      case WL_SEQWR: 
	glfs_fd_p = glfs_creat(glfs_p, next_fname, O_CREAT|O_EXCL|O_WRONLY|o_direct, 0666 );
        if ((!glfs_fd_p) && (errno == ENOENT)) {
          char subdir[1024];
          strcpy(subdir, dirname(next_fname));
          rc = glfs_mkdir(glfs_p, subdir, 0755);
          if (rc < OK) scallerr(subdir);
          get_next_path( k, files_per_dir, thrd_basedir, next_fname );
          glfs_fd_p = glfs_creat(glfs_p, next_fname, O_CREAT|O_EXCL|O_WRONLY|o_direct, 0666);
        }
	if (!glfs_fd_p) scallerr(next_fname);
	break;

      case WL_SEQRD:
        glfs_fd_p = glfs_open(glfs_p, next_fname, O_RDONLY|o_direct);
        if (!glfs_fd_p) scallerr(next_fname);
	break;

      case WL_RNDWR:
	glfs_fd_p = glfs_open(glfs_p, next_fname, O_WRONLY|o_direct);
	if (!glfs_fd_p) scallerr(next_fname);
	break;

      case WL_RNDRD:
	glfs_fd_p = glfs_open(glfs_p, next_fname, O_RDONLY|o_direct);
	if (!glfs_fd_p) scallerr(next_fname);
	break;

      default: exit(NOTOK);
     }
   }

   /* perform the requested I/O operations */

   offset = 0;
   if (debug) printf("io_requests = %ld\n", io_requests);
   FOREACH( io_count, io_requests ) {
    if (workload_type == WL_SEQWR) {
      offset += bytes_to_xfer;
      bytes_xferred = use_fuse ?
        write(fd, buf, bytes_to_xfer) :
        glfs_write(glfs_fd_p, buf, bytes_to_xfer, 0);
      if (bytes_xferred < bytes_to_xfer) 
		scallerr(use_fuse?"write":"glfs_write");

    } else if (workload_type == WL_SEQRD) {
      offset += bytes_to_xfer;
      bytes_xferred = use_fuse ? 
	read(fd, buf, bytes_to_xfer) :
	glfs_read(glfs_fd_p, buf, bytes_to_xfer, 0);
      if (bytes_xferred < bytes_to_xfer) 
		scallerr(use_fuse?"read":"glfs_read");

    } else if (workload_type == WL_RNDWR) {
      offset = random_offsets[io_count];
      bytes_xferred = use_fuse ?
	pwrite(fd, buf, bytes_to_xfer, offset) :
	glfs_pwrite(glfs_fd_p, buf, bytes_to_xfer, offset, 0);
      if (bytes_xferred < bytes_to_xfer) 
		scallerr(use_fuse?"pwrite":"glfs_pwrite");

    } else if (workload_type == WL_RNDRD) {
      offset = random_offsets[io_count];
      bytes_xferred = use_fuse ? 
	pread(fd, buf, bytes_to_xfer, offset) :
	glfs_pread(glfs_fd_p, buf, bytes_to_xfer, offset, 0);
      if (bytes_xferred < bytes_to_xfer) 
		scallerr(use_fuse?"pwrite":"glfs_pwrite");
    }
    total_bytes_xferred += bytes_xferred;
    if (debug) printf("offset %-20ld, io_count %-10u total_bytes_xferred %-20ld\n", 
                      offset, io_count, total_bytes_xferred);
   }
   total_io_count += io_count;

   /* shut down file access */

   if ((workload_type == WL_SEQWR || workload_type == WL_RNDWR) && fsync_at_close) {
     rc = use_fuse ? fsync(fd) : glfs_fsync(glfs_fd_p);
     if (rc) scallerr(use_fuse ? "fsync" : "glfs_fsync");
   }
   rc = use_fuse ? close(fd) : glfs_close(glfs_fd_p);
   if (rc) scallerr(use_fuse ? "close" : "glfs_close");
   files_done++;
  }
  end_time = gettime_ns();
  if (!use_fuse) glfs_fini(glfs_p);

  /* calculate and print stats */

  elapsed_time = end_time - start_time;
  if (debug) printf("start %ld end %ld elapsed %ld\n", start_time, end_time, elapsed_time);
  if (debug) printf("total byte count = "UINT64DFMT" total io count = "UINT64DFMT"\n", 
                     total_bytes_xferred, total_io_count );
  mb_transferred = (float )total_io_count * recsz / KB_PER_MB;
  thru = mb_transferred * NSEC_PER_SEC / elapsed_time ;
  files_thru = files_done * NSEC_PER_SEC / elapsed_time;
  if (files_done < 10) {
    files_thru = 0.0;
  }
  printf("file transfers  = "UINT64DFMT"\n", files_done);
  printf("I/O (record) transfers = "UINT64DFMT"\n", total_io_count);
  printf("total bytes = "UINT64DFMT"\n", total_bytes_xferred);
  printf("elapsed time    = %-9.2f sec\n", elapsed_time/NSEC_PER_SEC);
  printf("throughput      = %-9.2f MB/sec\n", thru);
  printf("file rate       = %-9.2f files/sec\n", files_thru);
  printf("IOPS            = %-9.2f (%s)\n", thru * 1024 / recsz, workload_description[workload_type]);
  return 0;
}
