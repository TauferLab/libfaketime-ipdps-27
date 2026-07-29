/*
 * Public API and shared-memory ABI for libfaketime's virtual realtime clock.
 *
 * Applications should use the functions below rather than access the shared
 * structure directly.  The structure is exposed here so its fixed-width,
 * versioned layout can be validated by the test suite.
 */
#ifndef FAKETIME_SHARED_CLOCK_H
#define FAKETIME_SHARED_CLOCK_H

#include <stdint.h>
#ifndef __cplusplus
#include <stdatomic.h>
#define FT_SHARED_ATOMIC(type) _Atomic type
#else
/* C++ callers use the functions below; the page remains an internal ABI. */
#define FT_SHARED_ATOMIC(type) type
#endif

#define FAKETIME_SHARED_CLOCK_ENV "FAKETIME_SHARED_CLOCK"
#define FAKETIME_SHARED_CLOCK_SHM_ENV "FAKETIME_SHARED_CLOCK_SHM"
#define FAKETIME_SHARED_CLOCK_INITIAL_NS_ENV \
  "FAKETIME_SHARED_CLOCK_INITIAL_NS"

#define FT_SHARED_CLOCK_MAGIC UINT32_C(0x4654434c) /* "FTCL" */
#define FT_SHARED_CLOCK_VERSION UINT32_C(1)
#define FT_SHARED_CLOCK_FLAG_ACTIVE UINT32_C(1)

/*
 * One writer publishes a new clock while arbitrarily many readers sample it.
 *
 * Payload fields are atomic as well as sequence.  A conventional seqlock with
 * non-atomic payload fields has a C data race when readers are threads in the
 * writer process.  Relaxed payload accesses avoid that race; the sequence's
 * acquire/release operations provide the publication ordering.
 */
struct ft_shared_virtual_clock
{
  uint32_t magic;
  uint32_t version;
  FT_SHARED_ATOMIC(uint64_t) sequence;
  FT_SHARED_ATOMIC(int64_t) realtime_real_base_ns;
  FT_SHARED_ATOMIC(int64_t) realtime_fake_base_ns;
  FT_SHARED_ATOMIC(int64_t) realtime_coarse_real_base_ns;
  FT_SHARED_ATOMIC(uint32_t) flags;
  uint32_t reserved;
  int64_t creator_pid;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Return 1 when this process is attached to a compatible active clock. */
int faketime_shared_clock_available(void);

/*
 * Publish target_ns as the current fake CLOCK_REALTIME value.
 *
 * Backward targets are accepted by this low-level API.  Controllers that need
 * monotonic simulated wall time must enforce that policy before calling it.
 * Returns 0 on success, or -1 with errno set when shared-clock mode is not
 * active, the layout is incompatible, or the target cannot be represented.
 */
int faketime_set_realtime_ns(int64_t target_ns);

#ifdef __cplusplus
}
#endif

#undef FT_SHARED_ATOMIC

#endif
