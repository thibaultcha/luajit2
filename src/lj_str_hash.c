/*
 * This file defines string hash function using CRC32.
 * On Intel architectures, this implemantation takes advantage of hardware
 * support (CRC32 instruction, SSE 4.2) to speedup the CRC32 computation.
 * On ARM64 architectures, this implementation utilizes the ARMv8.1-A extension
 * wich offers CRC32 instructions.
 * The hash functions try to compute CRC32 of length and up to 128 bytes of
 * the given string.
 */

#define lj_str_hash_c
#define LUA_CORE

#include "lj_str_hash.h"

#if LJ_OR_STRHASHCRC32

#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include "lj_vm.h"

#if LUAJIT_TARGET == LUAJIT_ARCH_X64
#include <smmintrin.h>

#define lj_crc32_u32 	_mm_crc32_u32
#define lj_crc32_u64 	_mm_crc32_u64

#ifndef F_CPU_SSE4_2
#define F_CPU_SSE4_2 	(1 << 20)
#endif

#elif LUAJIT_TARGET == LUAJIT_ARCH_ARM64
#include <sys/auxv.h>
#include <arm_acle.h>
#include <errno.h>

#define lj_crc32_u32 	__crc32cw
#define lj_crc32_u64 	__crc32cd

#ifndef HWCAP_CRC32
#define HWCAP_CRC32 	(1 << 7)
#endif

#else
#error "LJ_OR_STRHASHCRC32 not supported on this architecture"
#endif

#ifdef __MINGW32__
#define random() 	((long) rand())
#define srandom(seed) 	srand(seed)
#endif

static LJ_AINLINE const uint64_t* cast_uint64p(const char* str)
{
  return (const uint64_t*)(void*)str;
}

static LJ_AINLINE const uint32_t* cast_uint32p(const char* str)
{
  return (const uint32_t*)(void*)str;
}

/* hash string with len in [1, 4) */
static LJ_NOINLINE uint32_t lj_str_hash_1_4(const char* str, uint32_t len)
{
#if 0
  /* TODO: The if-1 part (i.e the original algorithm) is working better when
   * the load-factor is high, as revealed by conflict benchmark (via
   * 'make benchmark' command); need to understand why it's so.
   */
  uint32_t v = str[0];
  v = (v << 8) | str[len >> 1];
  v = (v << 8) | str[len - 1];
  v = (v << 8) | len;
  return lj_crc32_u32(0, v);
#else
  uint32_t a, b, h = len;

  a = *(const uint8_t *)str;
  h ^= *(const uint8_t *)(str+len-1);
  b = *(const uint8_t *)(str+(len>>1));
  h ^= b; h -= lj_rol(b, 14);

  a ^= h; a -= lj_rol(h, 11);
  b ^= a; b -= lj_rol(a, 25);
  h ^= b; h -= lj_rol(b, 16);

  return h;
#endif
}

/* hash string with len in [4, 16) */
static LJ_NOINLINE uint32_t lj_str_hash_4_16(const char* str, uint32_t len)
{
  uint64_t v1, v2, h;

  if (len >= 8) {
    v1 = *cast_uint64p(str);
    v2 = *cast_uint64p(str + len - 8);
  } else {
    v1 = *cast_uint32p(str);
    v2 = *cast_uint32p(str + len - 4);
  }

  h = lj_crc32_u32(0, len);
  h = lj_crc32_u64(h, v1);
  h = lj_crc32_u64(h, v2);

  return h;
}

/* hash string with length in [16, 128) */
static LJ_NOINLINE uint32_t lj_str_hash_16_128(const char* str, uint32_t len)
{
  uint64_t h1, h2;
  uint32_t i;

  h1 = lj_crc32_u32(0, len);
  h2 = 0;

  for (i = 0; i < len - 16; i += 16) {
    h1 += lj_crc32_u64(h1, *cast_uint64p(str + i));
    h2 += lj_crc32_u64(h2, *cast_uint64p(str + i + 8));
  };

  h1 = lj_crc32_u64(h1, *cast_uint64p(str + len - 16));
  h2 = lj_crc32_u64(h2, *cast_uint64p(str + len - 8));

  return lj_crc32_u32(h1, h2);
}

/* **************************************************************************
 *
 *  Following is code about hashing string with length >= 128
 *
 * **************************************************************************
 */
static uint32_t random_pos[32][2];
static const int8_t log2_tab[128] = { -1,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,
  4,4,4,4,4,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
  5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,6,6,6,6,6,6,6,6,6,6,6,6,
  6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
  6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6 };

/* return floor(log2(n)) */
static LJ_AINLINE uint32_t log2_floor(uint32_t n)
{
  if (n <= 127) {
    return log2_tab[n];
  }

  if ((n >> 8) <= 127) {
    return log2_tab[n >> 8] + 8;
  }

  if ((n >> 16) <= 127) {
    return log2_tab[n >> 16] + 16;
  }

  if ((n >> 24) <= 127) {
    return log2_tab[n >> 24] + 24;
  }

  return 31;
}

/* Return a pre-computed random number in the range of [1**chunk_sz_order,
 * 1**(chunk_sz_order+1)). It is "unsafe" in the sense that the return value
 * may be greater than chunk-size; it is up to the caller to make sure
 * "chunk-base + return-value-of-this-func" has valid virtual address.
 */
static LJ_AINLINE uint32_t get_random_pos_unsafe(uint32_t chunk_sz_order,
				 uint32_t idx)
{
  uint32_t pos = random_pos[chunk_sz_order][idx & 1];
  return pos;
}

static LJ_NOINLINE uint32_t lj_str_hash_128_above(const char* str,
				 uint32_t len)
{
  uint32_t chunk_num, chunk_sz, chunk_sz_log2, i, pos1, pos2;
  uint64_t h1, h2, v;
  const char* chunk_ptr;

  chunk_num = 16;
  chunk_sz = len / chunk_num;
  chunk_sz_log2 = log2_floor(chunk_sz);

  pos1 = get_random_pos_unsafe(chunk_sz_log2, 0);
  pos2 = get_random_pos_unsafe(chunk_sz_log2, 1);

  h1 = lj_crc32_u32(0, len);
  h2 = 0;

  /* loop over 14 chunks, 2 chunks at a time */
  for (i = 0, chunk_ptr = str; i < (chunk_num / 2 - 1);
       chunk_ptr += chunk_sz, i++) {

    v = *cast_uint64p(chunk_ptr + pos1);
    h1 = lj_crc32_u64(h1, v);

    v = *cast_uint64p(chunk_ptr + chunk_sz + pos2);
    h2 = lj_crc32_u64(h2, v);
  }

  /* the last two chunks */
  v = *cast_uint64p(chunk_ptr + pos1);
  h1 = lj_crc32_u64(h1, v);

  v = *cast_uint64p(chunk_ptr + chunk_sz - 8 - pos2);
  h2 = lj_crc32_u64(h2, v);

  /* process the trailing part */
  h1 = lj_crc32_u64(h1, *cast_uint64p(str));
  h2 = lj_crc32_u64(h2, *cast_uint64p(str + len - 8));

  h1 = lj_crc32_u32(h1, h2);

  return h1;
}

/* NOTE: the "len" should not be zero */
static MSize lj_str_hash_crc32(const char *str, size_t len)
{
  if (len < 128) {
    if (len >= 16) { /* [16, 128) */
      return lj_str_hash_16_128(str, len);
    }

    if (len >= 4) { /* [4, 16) */
      return lj_str_hash_4_16(str, len);
    }

    /* [0, 4) */
    return lj_str_hash_1_4(str, len);
  }

  /* [128, inf) */
  return lj_str_hash_128_above(str, len);
}

#define POW2_MASK(n) ((1L << (n)) - 1)

/* This function is to populate `random_pos` such that random_pos[i][*]
 * contains random value in the range of [2**i, 2**(i+1)).
 */
static void lj_str_hash_init_random(void)
{
  int i, seed, rml;

  /* Calculate the ceil(log2(RAND_MAX)) */
  rml = log2_floor(RAND_MAX);
  if (RAND_MAX & (RAND_MAX - 1)) {
    rml += 1;
  }

  /* Init seed */
  seed = lj_crc32_u32(0, getpid());
  seed = lj_crc32_u32(seed, time(NULL));
  srandom(seed);

  /* Now start to populate the random_pos[][]. */
  for (i = 0; i < 3; i++) {
    /* No need to provide random value for chunk smaller than 8 bytes */
    random_pos[i][0] = random_pos[i][1] = 0;
  }

  for (; i < rml; i++) {
    random_pos[i][0] = random() & POW2_MASK(i+1);
    random_pos[i][1] = random() & POW2_MASK(i+1);
  }

  for (; i < 31; i++) {
    int j;
    for (j = 0; j < 2; j++) {
      uint32_t v, scale;
      scale = random_pos[i - rml][0];
      if (scale == 0) {
        scale = 1;
      }
      v = (random() * scale) & POW2_MASK(i+1);
      random_pos[i][j] = v;
    }
  }
}

#undef POW2_MASK

LJ_FUNC unsigned char lj_check_crc32_support()
{
#if LUAJIT_TARGET == LUAJIT_ARCH_X64
  uint32_t features[4];
  if (lj_vm_cpuid(1, features))
    return (features[2] & F_CPU_SSE4_2) != 0;
#elif LUAJIT_TARGET == LUAJIT_ARCH_ARM64
  uint32_t hwcap = getauxval(AT_HWCAP);
  if (hwcap != ENOENT)
    return (hwcap & HWCAP_CRC32) != 0;
#endif
  return 0;
}

LJ_FUNC void lj_init_strhashfn(global_State *g)
{
  static StrHashFunction strhashfn;
  if (strhashfn == NULL) {
    if (lj_check_crc32_support()) {
      lj_str_hash_init_random();
      strhashfn = lj_str_hash_crc32;
    } else {
      strhashfn = lj_str_hash_orig;
    }
  }
  g->strhashfn = strhashfn;
}

#endif

LJ_FUNC MSize lj_str_hash_orig(const char *str, size_t lenx)
{
  MSize len = (MSize)lenx;
  MSize a, b, h = len;

  /* Compute string hash. Constants taken from lookup3 hash by Bob Jenkins. */
  if (len >= 4) {  /* Caveat: unaligned access! */
    a = lj_getu32(str);
    h ^= lj_getu32(str+len-4);
    b = lj_getu32(str+(len>>1)-2);
    h ^= b; h -= lj_rol(b, 14);
    b += lj_getu32(str+(len>>2)-1);
  } else if (len > 0) {
    a = *(const uint8_t *)str;
    h ^= *(const uint8_t *)(str+len-1);
    b = *(const uint8_t *)(str+(len>>1));
    h ^= b; h -= lj_rol(b, 14);
  } else {
    return 0;
  }

  a ^= h; a -= lj_rol(h, 11);
  b ^= a; b -= lj_rol(a, 25);
  h ^= b; h -= lj_rol(b, 16);

  return h;
}

