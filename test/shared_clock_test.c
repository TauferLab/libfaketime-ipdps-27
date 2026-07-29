#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/faketime_shared_clock.h"

#define NSEC_PER_SEC INT64_C(1000000000)
#define MAX_CLOCK_ERROR_NS INT64_C(2000000000)

typedef int (*available_fn)(void);
typedef int (*set_realtime_fn)(int64_t);

static available_fn shared_available;
static set_realtime_fn set_realtime_ns;
static const char *program_path;
static _Atomic int reader_failure;

static void fail(const char *message)
{
  fprintf(stderr, "FAIL: %s: %s\n", message, strerror(errno));
  exit(EXIT_FAILURE);
}

static int64_t timespec_ns(const struct timespec *ts)
{
  return ((int64_t)ts->tv_sec * NSEC_PER_SEC) + ts->tv_nsec;
}

static int64_t realtime_ns(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
  {
    fail("clock_gettime");
  }
  return timespec_ns(&ts);
}

static void assert_near(const char *label, int64_t actual, int64_t expected,
                        int64_t tolerance)
{
  int64_t difference = actual - expected;
  if (difference < 0)
  {
    difference = -difference;
  }
  if (difference > tolerance)
  {
    fprintf(stderr,
            "FAIL: %s actual=%lld expected=%lld difference=%lld ns\n",
            label, (long long)actual, (long long)expected,
            (long long)difference);
    exit(EXIT_FAILURE);
  }
}

static void load_api(void)
{
  shared_available = (available_fn)dlsym(RTLD_DEFAULT,
                                          "faketime_shared_clock_available");
  set_realtime_ns = (set_realtime_fn)dlsym(RTLD_DEFAULT,
                                            "faketime_set_realtime_ns");
  if ((shared_available == NULL) || (set_realtime_ns == NULL))
  {
    fprintf(stderr, "FAIL: shared clock API symbols are not visible: %s\n",
            dlerror());
    exit(EXIT_FAILURE);
  }
}

static void expect_update(int64_t target)
{
  if (set_realtime_ns(target) == -1)
  {
    fail("faketime_set_realtime_ns");
  }
  assert_near("updated CLOCK_REALTIME", realtime_ns(), target,
              MAX_CLOCK_ERROR_NS);
}

static void test_supported_apis(int64_t target)
{
  struct timespec ts;
  struct timeval tv;
  time_t seconds;

  expect_update(target);
  if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
  {
    fail("CLOCK_REALTIME");
  }
  assert_near("clock_gettime", timespec_ns(&ts), target,
              MAX_CLOCK_ERROR_NS);

#ifdef CLOCK_REALTIME_COARSE
  if (clock_gettime(CLOCK_REALTIME_COARSE, &ts) == -1)
  {
    fail("CLOCK_REALTIME_COARSE");
  }
  assert_near("clock_gettime coarse", timespec_ns(&ts), target,
              MAX_CLOCK_ERROR_NS);
#endif

  if (gettimeofday(&tv, NULL) == -1)
  {
    fail("gettimeofday");
  }
  assert_near("gettimeofday",
              ((int64_t)tv.tv_sec * NSEC_PER_SEC) +
                  ((int64_t)tv.tv_usec * 1000),
              target, MAX_CLOCK_ERROR_NS);

  seconds = time(NULL);
  if (seconds == (time_t)-1)
  {
    fail("time");
  }
  assert_near("time", (int64_t)seconds * NSEC_PER_SEC, target,
              MAX_CLOCK_ERROR_NS);
}

static void test_fork_propagation(int64_t target)
{
  int ready_pipe[2];
  int go_pipe[2];
  int result_pipe[2];
  int64_t child_time;
  char byte = 'x';
  pid_t child;
  int status;

  if ((pipe(ready_pipe) == -1) || (pipe(go_pipe) == -1) ||
      (pipe(result_pipe) == -1))
  {
    fail("pipe");
  }
  child = fork();
  if (child == -1)
  {
    fail("fork");
  }
  if (child == 0)
  {
    close(ready_pipe[0]);
    close(go_pipe[1]);
    close(result_pipe[0]);
    if ((write(ready_pipe[1], &byte, 1) != 1) ||
        (read(go_pipe[0], &byte, 1) != 1))
    {
      _exit(2);
    }
    child_time = realtime_ns();
    if (write(result_pipe[1], &child_time, sizeof(child_time)) !=
        (ssize_t)sizeof(child_time))
    {
      _exit(3);
    }
    _exit(0);
  }

  close(ready_pipe[1]);
  close(go_pipe[0]);
  close(result_pipe[1]);
  if (read(ready_pipe[0], &byte, 1) != 1)
  {
    fail("fork child ready");
  }
  expect_update(target);
  if ((write(go_pipe[1], &byte, 1) != 1) ||
      (read(result_pipe[0], &child_time, sizeof(child_time)) !=
       (ssize_t)sizeof(child_time)))
  {
    fail("fork synchronization");
  }
  if ((waitpid(child, &status, 0) != child) || !WIFEXITED(status) ||
      (WEXITSTATUS(status) != 0))
  {
    errno = ECHILD;
    fail("fork child status");
  }
  assert_near("fork child update", child_time, target, MAX_CLOCK_ERROR_NS);
}

static int exec_reader(int argc, char **argv)
{
  int read_fd;
  int write_fd;
  int64_t value;
  char byte = 'r';

  if (argc != 4)
  {
    return 64;
  }
  read_fd = atoi(argv[2]);
  write_fd = atoi(argv[3]);
  load_api();
  if (shared_available() != 1)
  {
    return 65;
  }
  if ((write(write_fd, &byte, 1) != 1) ||
      (read(read_fd, &byte, 1) != 1))
  {
    return 66;
  }
  value = realtime_ns();
  if (write(write_fd, &value, sizeof(value)) != (ssize_t)sizeof(value))
  {
    return 67;
  }
  return 0;
}

static void test_exec_propagation(int64_t target)
{
  int parent_to_child[2];
  int child_to_parent[2];
  char read_fd[32];
  char write_fd[32];
  char byte;
  int64_t child_time;
  pid_t child;
  int status;

  if ((pipe(parent_to_child) == -1) || (pipe(child_to_parent) == -1))
  {
    fail("exec pipes");
  }
  child = fork();
  if (child == -1)
  {
    fail("exec fork");
  }
  if (child == 0)
  {
    snprintf(read_fd, sizeof(read_fd), "%d", parent_to_child[0]);
    snprintf(write_fd, sizeof(write_fd), "%d", child_to_parent[1]);
    execl(program_path, program_path, "--exec-reader", read_fd, write_fd,
          (char *)NULL);
    _exit(127);
  }

  close(parent_to_child[0]);
  close(child_to_parent[1]);
  if (read(child_to_parent[0], &byte, 1) != 1)
  {
    fail("exec child ready");
  }
  expect_update(target);
  if ((write(parent_to_child[1], "g", 1) != 1) ||
      (read(child_to_parent[0], &child_time, sizeof(child_time)) !=
       (ssize_t)sizeof(child_time)))
  {
    fail("exec synchronization");
  }
  if ((waitpid(child, &status, 0) != child) || !WIFEXITED(status) ||
      (WEXITSTATUS(status) != 0))
  {
    errno = ECHILD;
    fail("exec child status");
  }
  assert_near("exec child update", child_time, target, MAX_CLOCK_ERROR_NS);
}

static void *reader_thread(void *unused)
{
  int64_t previous = INT64_MIN;
  int64_t current;
  int iteration;
  (void)unused;

  for (iteration = 0; iteration < 50000; iteration++)
  {
    current = realtime_ns();
    if (current < previous)
    {
      atomic_store_explicit(&reader_failure, 1, memory_order_relaxed);
      return NULL;
    }
    previous = current;
  }
  return NULL;
}

static void test_multiple_readers(int64_t target)
{
  pthread_t readers[4];
  int reader;
  int update;

  expect_update(target);
  atomic_store_explicit(&reader_failure, 0, memory_order_relaxed);
  for (reader = 0; reader < 4; reader++)
  {
    if (pthread_create(&readers[reader], NULL, reader_thread, NULL) != 0)
    {
      fail("pthread_create");
    }
  }
  for (update = 1; update <= 100; update++)
  {
    if (set_realtime_ns(target + ((int64_t)update * NSEC_PER_SEC)) == -1)
    {
      fail("concurrent clock update");
    }
  }
  for (reader = 0; reader < 4; reader++)
  {
    if (pthread_join(readers[reader], NULL) != 0)
    {
      fail("pthread_join");
    }
  }
  if (atomic_load_explicit(&reader_failure, memory_order_relaxed) != 0)
  {
    errno = EPROTO;
    fail("reader observed backward or torn time");
  }
}

static int expect_unavailable(void)
{
  load_api();
  errno = 0;
  if ((shared_available() != 0) || (errno != EPROTO))
  {
    fprintf(stderr,
            "FAIL: incompatible layout was not rejected (errno=%d)\n",
            errno);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

static void test_invalid_layout(void)
{
  char name[128];
  struct ft_shared_virtual_clock *page;
  int fd;
  pid_t child;
  int status;

  snprintf(name, sizeof(name), "/faketime_invalid_test_%ld", (long)getpid());
  fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd == -1)
  {
    fail("invalid-layout shm_open");
  }
  if (ftruncate(fd, sizeof(*page)) == -1)
  {
    fail("invalid-layout ftruncate");
  }
  page = mmap(NULL, sizeof(*page), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (page == MAP_FAILED)
  {
    fail("invalid-layout mmap");
  }
  memset(page, 0, sizeof(*page));
  page->magic = UINT32_C(0xdeadbeef);
  page->version = FT_SHARED_CLOCK_VERSION + 1;
  munmap(page, sizeof(*page));
  close(fd);

  child = fork();
  if (child == -1)
  {
    fail("invalid-layout fork");
  }
  if (child == 0)
  {
    if (setenv(FAKETIME_SHARED_CLOCK_SHM_ENV, name, 1) == -1)
    {
      _exit(126);
    }
    execl(program_path, program_path, "--expect-unavailable", (char *)NULL);
    _exit(127);
  }
  if ((waitpid(child, &status, 0) != child) || !WIFEXITED(status) ||
      (WEXITSTATUS(status) != 0))
  {
    shm_unlink(name);
    errno = EPROTO;
    fail("invalid layout child status");
  }
  if (shm_unlink(name) == -1)
  {
    fail("invalid-layout shm_unlink");
  }
}

int main(int argc, char **argv)
{
  int64_t base = INT64_C(946684800000000000);
  program_path = argv[0];

  if ((argc > 1) && (strcmp(argv[1], "--exec-reader") == 0))
  {
    return exec_reader(argc, argv);
  }
  if ((argc > 1) && (strcmp(argv[1], "--expect-unavailable") == 0))
  {
    return expect_unavailable();
  }

  load_api();
  if (shared_available() != 1)
  {
    fail("shared clock is unavailable");
  }
  assert_near("initial target", realtime_ns(), base, MAX_CLOCK_ERROR_NS);
  test_supported_apis(base + (10 * NSEC_PER_SEC));
  test_fork_propagation(base + (20 * NSEC_PER_SEC));
  test_exec_propagation(base + (30 * NSEC_PER_SEC));
  test_multiple_readers(base + (40 * NSEC_PER_SEC));
  test_invalid_layout();
  puts("PASS: shared clock single-process, APIs, fork, exec, readers, layout");
  return EXIT_SUCCESS;
}
