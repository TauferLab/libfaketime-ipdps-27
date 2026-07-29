#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NSEC_PER_SEC INT64_C(1000000000)

typedef int (*available_fn)(void);
typedef int (*set_realtime_fn)(int64_t);

static set_realtime_fn shared_set_realtime;

static int raw_clock_gettime(clockid_t clock_id, struct timespec *ts)
{
#ifdef SYS_clock_gettime
  return (int)syscall(SYS_clock_gettime, clock_id, ts);
#else
  return clock_gettime(clock_id, ts);
#endif
}

static int64_t timespec_ns(const struct timespec *ts)
{
  return ((int64_t)ts->tv_sec * NSEC_PER_SEC) + ts->tv_nsec;
}

static int64_t parse_i64_env(const char *name)
{
  const char *text = getenv(name);
  char *end = NULL;
  intmax_t value;

  if ((text == NULL) || (*text == '\0'))
  {
    fprintf(stderr, "%s is required\n", name);
    exit(EXIT_FAILURE);
  }
  errno = 0;
  value = strtoimax(text, &end, 10);
  if ((errno != 0) || (end == text) || (*end != '\0'))
  {
    fprintf(stderr, "invalid %s=%s\n", name, text);
    exit(EXIT_FAILURE);
  }
  return (int64_t)value;
}

static int write_all(int fd, const char *buffer, size_t size)
{
  while (size > 0)
  {
    ssize_t written = write(fd, buffer, size);
    if (written < 0)
    {
      return -1;
    }
    buffer += written;
    size -= (size_t)written;
  }
  return 0;
}

static int legacy_update(const char *path, int64_t target_ns)
{
  struct timespec real_ts;
  char temporary[4096];
  char contents[128];
  uint64_t magnitude;
  uint64_t seconds;
  uint64_t nanoseconds;
  int64_t offset_ns;
  int length;
  int fd;
  char sign;

  if (raw_clock_gettime(CLOCK_REALTIME, &real_ts) == -1)
  {
    return -1;
  }
  offset_ns = target_ns - timespec_ns(&real_ts);
  sign = offset_ns < 0 ? '-' : '+';
  magnitude = offset_ns < 0
                  ? (uint64_t)(-(offset_ns + 1)) + UINT64_C(1)
                  : (uint64_t)offset_ns;
  seconds = magnitude / NSEC_PER_SEC;
  nanoseconds = magnitude % NSEC_PER_SEC;
  length = snprintf(contents, sizeof(contents), "%c%" PRIu64 ".%09" PRIu64
                    "s\n", sign, seconds, nanoseconds);
  if ((length < 0) || ((size_t)length >= sizeof(contents)))
  {
    errno = EOVERFLOW;
    return -1;
  }
  if (snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", path,
               (long)getpid()) >= (int)sizeof(temporary))
  {
    errno = ENAMETOOLONG;
    return -1;
  }
  fd = open(temporary, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
  if (fd == -1)
  {
    return -1;
  }
  if ((write_all(fd, contents, (size_t)length) == -1) || (close(fd) == -1))
  {
    int saved_errno = errno;
    unlink(temporary);
    errno = saved_errno;
    return -1;
  }
  if (rename(temporary, path) == -1)
  {
    int saved_errno = errno;
    unlink(temporary);
    errno = saved_errno;
    return -1;
  }
  return 0;
}

static int prepare_shared_update(void)
{
  available_fn available = (available_fn)dlsym(
      RTLD_DEFAULT, "faketime_shared_clock_available");
  shared_set_realtime = (set_realtime_fn)dlsym(
      RTLD_DEFAULT, "faketime_set_realtime_ns");

  if ((available == NULL) || (shared_set_realtime == NULL))
  {
    fprintf(stderr, "shared clock symbols unavailable: %s\n", dlerror());
    errno = ENOSYS;
    return -1;
  }
  if (available() != 1)
  {
    errno = ENODEV;
    return -1;
  }
  return 0;
}

static int shared_update(int64_t target_ns)
{
  return shared_set_realtime(target_ns);
}

int main(int argc, char **argv)
{
  const char *mode = getenv("BENCH_MODE");
  const char *legacy_file = getenv("BENCH_LEGACY_FILE");
  struct timespec started;
  struct timespec finished;
  struct timespec observed;
  volatile uint64_t sink = 0;
  uint64_t calls;
  uint64_t warmup;
  uint64_t iteration;
  uint64_t update_calls = 1000;
  int64_t target_ns = 0;
  int64_t update_ns = 0;
  int64_t total_ns;
  int expects_fake;

  if ((argc != 2) || (mode == NULL))
  {
    fprintf(stderr, "usage: BENCH_MODE=... %s CALLS\n", argv[0]);
    return EXIT_FAILURE;
  }
  calls = strtoull(argv[1], NULL, 10);
  if (calls == 0)
  {
    fprintf(stderr, "CALLS must be positive\n");
    return EXIT_FAILURE;
  }
  expects_fake = strcmp(mode, "native") != 0;
  if (expects_fake)
  {
    const char *update_calls_text = getenv("BENCH_UPDATE_CALLS");
    if (update_calls_text != NULL)
    {
      update_calls = strtoull(update_calls_text, NULL, 10);
      if (update_calls == 0)
      {
        fprintf(stderr, "BENCH_UPDATE_CALLS must be positive\n");
        return EXIT_FAILURE;
      }
    }
    target_ns = parse_i64_env("BENCH_TARGET_NS");
    if ((strcmp(mode, "shared") == 0) && (prepare_shared_update() == -1))
    {
      perror("prepare shared update");
      return EXIT_FAILURE;
    }
    if (raw_clock_gettime(CLOCK_MONOTONIC_RAW, &started) == -1)
    {
      perror("update start clock");
      return EXIT_FAILURE;
    }
    for (iteration = 0; iteration < update_calls; iteration++)
    {
      if (strcmp(mode, "legacy") == 0)
      {
        if ((legacy_file == NULL) ||
            (legacy_update(legacy_file, target_ns) == -1))
        {
          perror("legacy update");
          return EXIT_FAILURE;
        }
      }
      else if (strcmp(mode, "shared") == 0)
      {
        if (shared_update(target_ns) == -1)
        {
          perror("shared update");
          return EXIT_FAILURE;
        }
      }
      else
      {
        fprintf(stderr, "unknown BENCH_MODE=%s\n", mode);
        return EXIT_FAILURE;
      }
    }
    if (raw_clock_gettime(CLOCK_MONOTONIC_RAW, &finished) == -1)
    {
      perror("update end clock");
      return EXIT_FAILURE;
    }
    update_ns = (timespec_ns(&finished) - timespec_ns(&started)) /
                (int64_t)update_calls;
  }

  if (clock_gettime(CLOCK_REALTIME, &observed) == -1)
  {
    perror("correctness clock_gettime");
    return EXIT_FAILURE;
  }
  if (expects_fake)
  {
    int64_t error_ns = timespec_ns(&observed) - target_ns;
    if (error_ns < 0)
    {
      error_ns = -error_ns;
    }
    if (error_ns > (2 * NSEC_PER_SEC))
    {
      fprintf(stderr,
              "%s did not observe dynamic fake target: error=%" PRId64
              " ns\n", mode, error_ns);
      return EXIT_FAILURE;
    }
  }

  warmup = calls < 100000 ? calls : 100000;
  for (iteration = 0; iteration < warmup; iteration++)
  {
    if (clock_gettime(CLOCK_REALTIME, &observed) == -1)
    {
      perror("warmup clock_gettime");
      return EXIT_FAILURE;
    }
    sink += (uint64_t)observed.tv_nsec;
  }

  /* strace audits use this marker to ignore initialization and warm-up. */
  if (write(STDERR_FILENO, "BENCHMARK_HOT_PATH_BEGIN\n", 25) != 25)
  {
    return EXIT_FAILURE;
  }
  if (raw_clock_gettime(CLOCK_MONOTONIC_RAW, &started) == -1)
  {
    perror("benchmark start clock");
    return EXIT_FAILURE;
  }
  for (iteration = 0; iteration < calls; iteration++)
  {
    if (clock_gettime(CLOCK_REALTIME, &observed) == -1)
    {
      perror("benchmark clock_gettime");
      return EXIT_FAILURE;
    }
    sink += (uint64_t)observed.tv_nsec;
  }
  if (raw_clock_gettime(CLOCK_MONOTONIC_RAW, &finished) == -1)
  {
    perror("benchmark end clock");
    return EXIT_FAILURE;
  }

  total_ns = timespec_ns(&finished) - timespec_ns(&started);
  if (write(STDERR_FILENO, "BENCHMARK_HOT_PATH_END\n", 23) != 23)
  {
    return EXIT_FAILURE;
  }
  printf("%" PRIu64 ",%" PRId64 ",%.6f,%" PRId64 ",%" PRId64
         ",%" PRIu64 "\n",
         calls, total_ns, (double)total_ns / (double)calls, update_ns,
         timespec_ns(&observed), (uint64_t)sink);
  return EXIT_SUCCESS;
}
