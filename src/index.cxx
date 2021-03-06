/*
 * Copyright 2016-2017 libfpta authors: please see AUTHORS file.
 *
 * This file is part of libfpta, aka "Fast Positive Tables".
 *
 * libfpta is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libfpta is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libfpta.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "fast_positive/tables_internal.h"

//----------------------------------------------------------------------------

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__) ||           \
    !defined(__ORDER_BIG_ENDIAN__)
#error __BYTE_ORDER__ should be defined.
#endif

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ &&                               \
    __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error Unsupported byte order.
#endif

#if !defined(UNALIGNED_OK)
#if defined(__i386) || defined(__x86_64) || defined(_M_IX86) || defined(_M_X64)
#define UNALIGNED_OK 1
#else
#define UNALIGNED_OK 0
#endif
#endif

static int __hot fpta_idxcmp_binary_last2first(const MDB_val *a,
                                               const MDB_val *b) {
  const uint8_t *pa = (const uint8_t *)a->mv_data + a->mv_size;
  const uint8_t *pb = (const uint8_t *)b->mv_data + b->mv_size;
  const size_t shortest = (a->mv_size < b->mv_size) ? a->mv_size : b->mv_size;
  const uint8_t *const stopper = pa - shortest;

#if UNALIGNED_OK && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  if (shortest >= sizeof(size_t)) {
    do {
      pa -= sizeof(size_t);
      pb -= sizeof(size_t);
      int diff = fptu_cmp2int(*(size_t *)pa, *(size_t *)pb);
      if (likely(diff))
        return diff;
    } while (pa - sizeof(size_t) >= stopper);
  }
  if (sizeof(unsigned) < sizeof(size_t) && pa >= stopper + sizeof(unsigned)) {
    pa -= sizeof(unsigned);
    pb -= sizeof(unsigned);
    int diff = fptu_cmp2int(*(unsigned *)pa, *(unsigned *)pb);
    if (likely(diff))
      return diff;
  }
  if (sizeof(unsigned short) < sizeof(unsigned) &&
      pa >= stopper + sizeof(unsigned short)) {
    pa -= sizeof(unsigned short);
    pb -= sizeof(unsigned short);
    int diff = *(unsigned short *)pa - *(unsigned short *)pb;
    if (likely(diff))
      return diff;
  }
#endif

  while (pa != stopper) {
    int diff = *--pa - *--pb;
    if (likely(diff))
      return diff;
  }
  return fptu_cmp2int(a->mv_size, b->mv_size);
}

static int __hot fpta_idxcmp_binary_first2last(const MDB_val *a,
                                               const MDB_val *b) {
  size_t shortest = (a->mv_size < b->mv_size) ? a->mv_size : b->mv_size;
  int diff = memcmp(a->mv_data, b->mv_data, shortest);
  return likely(diff) ? diff : fptu_cmp2int(a->mv_size, b->mv_size);
}

template <typename T>
static int __hot fpta_idxcmp_type(const MDB_val *a, const MDB_val *b) {
  assert(a->mv_size == sizeof(T) && b->mv_size == sizeof(T));
  if (UNALIGNED_OK || sizeof(T) < 4) {
    const T va = *(const T *)a->mv_data;
    const T vb = *(const T *)b->mv_data;
    return fptu_cmp2int(va, vb);
  } else if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) {
    const uint8_t *pa = (const uint8_t *)a->mv_data;
    const uint8_t *pb = (const uint8_t *)b->mv_data;
    int diff, i = sizeof(T) - 1;
    do
      diff = pa[i] - pb[i];
    while (diff == 0 && --i >= 0);
    return diff;
  } else if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {
    return memcmp(a->mv_data, b->mv_data, sizeof(T));
  }
  return 0;
}

static int __hot fpta_idxcmp_fp32(const MDB_val *a, const MDB_val *b) {
  assert(a->mv_size == 4 && b->mv_size == 4);
  int32_t va, vb;

#if UNALIGNED_OK
  va = *(const int32_t *)a->mv_data;
  vb = *(const int32_t *)b->mv_data;
#else
  memcpy(va, a->mv_data, 4);
  memcpy(vb, b->mv_data, 4);
#endif

  int32_t negative = va & (1 << 31);
  if ((negative ^ vb) < 0)
    return negative ? -1 : 1;

  int32_t cmp = (va & 0x7fffFFFF) - (vb & 0x7fffFFFF);
  return negative ? -cmp : cmp;
}

static int __hot fpta_idxcmp_fp64(const MDB_val *a, const MDB_val *b) {
  assert(a->mv_size == 8 && b->mv_size == 8);
  int64_t va, vb;

#if UNALIGNED_OK
  va = *(const int64_t *)a->mv_data;
  vb = *(const int64_t *)b->mv_data;
#else
  memcpy(va, a->mv_data, 8);
  memcpy(vb, b->mv_data, 8);
#endif

  int64_t negative = va & 0x8000000000000000ll;
  if ((negative ^ vb) < 0)
    return negative ? -1 : 1;

  int cmp = fptu_cmp2int(va & 0x7fffFFFFffffFFFFll, vb & 0x7fffFFFFffffFFFFll);
  return negative ? -cmp : cmp;
}

static int fpta_idxcmp_tuple(const MDB_val *a, const MDB_val *b) {
  switch (fptu_cmp_tuples(*(const fptu_ro *)a, *(const fptu_ro *)b)) {
  case fptu_eq:
    return 0;
  case fptu_lt:
    return -1;
  case fptu_gt:
    return 1;
  default:
    assert(0 && "incomparable tuples");
    return 42;
  }
}

static int fpta_idxcmp_mad(const MDB_val *a, const MDB_val *b) {
  (void)a;
  (void)b;
  return 0;
}

__hot MDB_cmp_func *fpta_index_shove2comparator(fpta_shove_t shove) {
  fptu_type type = fpta_shove2type(shove);
  fpta_index_type index = fpta_shove2index(shove);

  switch (type) {
  default:
    if (type >= fptu_96) {
      if (!fpta_index_is_ordered(index))
        return fpta_idxcmp_type<uint64_t>;
      if (fpta_index_is_reverse(index))
        return fpta_idxcmp_binary_last2first;
      return fpta_idxcmp_binary_first2last;
    }
    assert(0 && "wrong type for index");
    return fpta_idxcmp_mad;

  case fptu_nested:
    return fpta_idxcmp_tuple;
  case fptu_fp32:
    static_assert(sizeof(float) == sizeof(int32_t), "something wrong");
    static_assert(sizeof(int) == 4, "something wrong");
    return fpta_idxcmp_fp32;
  case fptu_int32:
    return fpta_idxcmp_type<int32_t>;
  case fptu_uint32:
  case fptu_uint16:
    return fpta_idxcmp_type<uint32_t>;
  case fptu_fp64:
    static_assert(sizeof(double) == sizeof(int64_t), "something wrong");
    static_assert(sizeof(int64_t) == 8, "something wrong");
    return fpta_idxcmp_fp64;
  case fptu_int64:
    return fpta_idxcmp_type<int64_t>;
  case fptu_uint64:
  case fptu_datetime:
    return fpta_idxcmp_type<uint64_t>;
  }
}

void *__fpta_index_shove2comparator(fpta_shove_t shove) {
  return (void *)fpta_index_shove2comparator(shove);
}

static __hot int fpta_normalize_key(fpta_shove_t shove, fpta_key &key,
                                    bool copy) {
  static_assert(fpta_max_keylen % sizeof(uint64_t) == 0,
                "wrong fpta_max_keylen");

  assert(key.mdbx.mv_data != &key.place);
  if (unlikely(key.mdbx.mv_data == nullptr) && key.mdbx.mv_size)
    return FPTA_EINVAL;

  if (!fpta_index_is_ordered(shove)) {
    // хешируем ключ для неупорядоченного индекса
    key.place.u64 = t1ha(key.mdbx.iov_base, key.mdbx.iov_len, 2017);
    key.mdbx.iov_base = &key.place.u64;
    key.mdbx.iov_len = sizeof(key.place.u64);
    return FPTA_SUCCESS;
  }

  void *buffer = fpta_index_is_reverse(shove) ? key.place.longkey_lsb.tail
                                              : key.place.longkey_msb.head;
  if (likely(key.mdbx.mv_size <= fpta_max_keylen)) {
    if (copy) {
      memcpy(buffer, key.mdbx.mv_data, key.mdbx.mv_size);
      key.mdbx.mv_data = buffer;
    }
    return FPTA_SUCCESS;
  }

  /* ключ слишком большой, сохраняем сколько допустимо остальное хэшируем */
  static_assert(sizeof(key.place.longkey_msb.head) ==
                    sizeof(key.place.longkey_lsb.tail),
                "something wrong");
  static_assert(fpta_max_keylen == sizeof(key.place.longkey_msb.head),
                "something wrong");

  if (!fpta_index_is_reverse(shove)) {
    /* ключ сравнивается от головы к хвосту (как memcpy),
     * копируем начало и хэшируем хвост. */
    memcpy(buffer, key.mdbx.mv_data, fpta_max_keylen);
    key.place.longkey_msb.tailhash =
        t1ha((const uint8_t *)key.mdbx.mv_data + fpta_max_keylen,
             key.mdbx.mv_size - fpta_max_keylen, 0);
  } else {
    /* ключ сравнивается от хвоста к голове, копируем хвост и хэшируем
     * начало. */
    key.place.longkey_lsb.headhash =
        t1ha((const uint8_t *)key.mdbx.mv_data,
             key.mdbx.mv_size - fpta_max_keylen, 0);
    memcpy(buffer, (const uint8_t *)key.mdbx.mv_data + key.mdbx.mv_size -
                       fpta_max_keylen,
           fpta_max_keylen);
  }

  static_assert(sizeof(key.place.longkey_msb) == sizeof(key.place.longkey_lsb),
                "something wrong");
  key.mdbx.mv_size = sizeof(key.place.longkey_msb);
  key.mdbx.mv_data = &key.place.longkey_msb;
  return FPTA_SUCCESS;
}

//----------------------------------------------------------------------------

static __inline unsigned shove2dbiflags(fpta_shove_t shove) {
  fptu_type type = fpta_shove2type(shove);
  fpta_index_type index = fpta_shove2index(shove);
  assert(type != fptu_null);
  assert(index != fpta_index_none);

  unsigned dbi_flags = fpta_index_is_unique(index) ? 0u : (unsigned)MDB_DUPSORT;
  if (type < fptu_96 || !fpta_index_is_ordered(index))
    dbi_flags |= MDB_INTEGERKEY;
  else if (fpta_index_is_reverse(index))
    dbi_flags |= MDB_REVERSEKEY;

  return dbi_flags | MDB_CREATE;
}

unsigned fpta_index_shove2primary_dbiflags(fpta_shove_t shove) {
  assert(fpta_index_is_primary(fpta_shove2index(shove)));
  return shove2dbiflags(shove);
}

unsigned fpta_index_shove2secondary_dbiflags(fpta_shove_t pk_shove,
                                             fpta_shove_t shove) {
  assert(fpta_index_is_primary(fpta_shove2index(pk_shove)));
  assert(fpta_index_is_secondary(fpta_shove2index(shove)));

  fptu_type pk_type = fpta_shove2type(pk_shove);
  fpta_index_type pk_index = fpta_shove2index(pk_shove);
  unsigned dbi_flags = shove2dbiflags(shove);
  if (dbi_flags & MDB_DUPSORT) {
    if (pk_type < fptu_cstr)
      dbi_flags |= MDB_DUPFIXED;
    if (pk_type < fptu_96 || !fpta_index_is_ordered(pk_index))
      dbi_flags |= MDB_INTEGERDUP;
    else if (fpta_index_is_reverse(pk_index))
      dbi_flags |= MDB_REVERSEDUP;
  }
  return dbi_flags;
}

static bool fpta_index_ordered_is_compat(fptu_type data_type,
                                         fpta_value_type value_type) {
  /* Критерий сравнимости:
   *  - все индексы коротких типов (использующие MDB_INTEGERKEY) могут быть
   *    использованы только со значениями РАВНОГО фиксированного размера.
   *  - МОЖНО "смешивать" signed и unsigned, так как fpta_index_value2key()
   *    преобразует значение, либо вернет ошибку.
   *  - но НЕ допускается смешивать integer и float.
   *  - shoved допустим только при возможности больших ключей.
   */
  static int32_t bits[fpta_end + 1] = {
      /* fpta_null */
      0,

      /* fpta_signed_int */
      1 << fptu_uint16 | 1 << fptu_uint32 | 1 << fptu_uint64 | 1 << fptu_int32 |
          1 << fptu_int64,

      /* fpta_unsigned_int */
      1 << fptu_uint16 | 1 << fptu_uint32 | 1 << fptu_uint64 | 1 << fptu_int32 |
          1 << fptu_int64,

      /* fpta_datetime */
      1 << fptu_datetime,

      /* fpta_float_point */
      1 << fptu_fp32 | 1 << fptu_fp64,

      /* fpta_string */
      1 << fptu_cstr,

      /* fpta_binary */
      ~(1 << fptu_null | 1 << fptu_int32 | 1 << fptu_int64 |
        1 << fptu_datetime | 1 << fptu_uint16 | 1 << fptu_uint32 |
        1 << fptu_uint64 | 1 << fptu_fp32 | 1 << fptu_fp64 | 1 << fptu_cstr),

      /* fpta_shoved */
      ~(1 << fptu_null | 1 << fptu_int32 | 1 << fptu_int64 |
        1 << fptu_datetime | 1 << fptu_uint16 | 1 << fptu_uint32 |
        1 << fptu_uint64 | 1 << fptu_fp32 | 1 << fptu_fp64 | 1 << fptu_96 |
        1 << fptu_128 | 1 << fptu_160 | 1 << fptu_256),

      /* fpta_begin */
      ~(1 << fptu_null),

      /* fpta_end */
      ~(1 << fptu_null)};

  return (bits[value_type] & (1 << data_type)) != 0;
}

static bool fpta_index_unordered_is_compat(fptu_type data_type,
                                           fpta_value_type value_type) {
  /* Критерий сравнимости:
   *  - все индексы коротких типов (использующие MDB_INTEGERKEY) могут быть
   *    использованы только со значениями РАВНОГО фиксированного размера.
   *  - МОЖНО "смешивать" signed и unsigned, так как fpta_index_value2key()
   *    преобразует значение, либо вернет ошибку.
   *  - но НЕ допускается смешивать integer и float.
   *  - shoved для всех типов, которые могут быть длиннее 8. */
  static int32_t bits[fpta_end + 1] = {
      /* fpta_null */
      0,

      /* fpta_signed_int */
      1 << fptu_uint16 | 1 << fptu_uint32 | 1 << fptu_uint64 | 1 << fptu_int32 |
          1 << fptu_int64,

      /* fpta_unsigned_int */
      1 << fptu_uint16 | 1 << fptu_uint32 | 1 << fptu_uint64 | 1 << fptu_int32 |
          1 << fptu_int64,

      /* fpta_date_time */
      1 << fptu_datetime,

      /* fpta_float_point */
      1 << fptu_fp32 | 1 << fptu_fp64,

      /* fpta_string */
      1 << fptu_cstr,

      /* fpta_binary */
      ~(1 << fptu_null | 1 << fptu_int32 | 1 << fptu_int64 |
        1 << fptu_datetime | 1 << fptu_uint16 | 1 << fptu_uint32 |
        1 << fptu_uint64 | 1 << fptu_fp32 | 1 << fptu_fp64 | 1 << fptu_cstr),

      /* fpta_shoved */
      ~(1 << fptu_null | 1 << fptu_int32 | 1 << fptu_int64 |
        1 << fptu_datetime | 1 << fptu_uint16 | 1 << fptu_uint32 |
        1 << fptu_uint64 | 1 << fptu_fp32 | 1 << fptu_fp64),

      /* fpta_begin */
      ~(1 << fptu_null),

      /* fpta_end */
      ~(1 << fptu_null)};

  return (bits[value_type] & (1 << data_type)) != 0;
}

bool fpta_index_is_compat(fpta_shove_t shove, const fpta_value &value) {
  fptu_type type = fpta_shove2type(shove);
  fpta_index_type index = fpta_shove2index(shove);

  if (fpta_index_is_ordered(index))
    return fpta_index_ordered_is_compat(type, value.type);

  return fpta_index_unordered_is_compat(type, value.type);
}

//----------------------------------------------------------------------------

int fpta_index_value2key(fpta_shove_t shove, const fpta_value &value,
                         fpta_key &key, bool copy) {
  if (unlikely(value.type == fpta_begin || value.type == fpta_end ||
               value.type == fpta_null))
    return FPTA_ETYPE;

  fptu_type type = fpta_shove2type(shove);
  fpta_index_type index = fpta_shove2index(shove);

  if (unlikely(index == fpta_index_none || type == fptu_null))
    return FPTA_EOOPS;

  if (fpta_index_is_ordered(index)) {
    // упорядоченный индекс
    if (unlikely(!fpta_index_ordered_is_compat(type, value.type)))
      return FPTA_ETYPE;

    if (value.type == fpta_shoved) {
      if (unlikely(value.binary_length != sizeof(key.place)))
        return FPTA_DATALEN_MISMATCH;
      if (unlikely(value.binary_data == nullptr))
        return FPTA_EINVAL;

      key.mdbx.mv_size = sizeof(key.place);
      key.mdbx.mv_data = value.binary_data;
      if (copy) {
        memcpy(&key.place, key.mdbx.mv_data, sizeof(key.place));
        key.mdbx.mv_data = &key.place;
      }
      return FPTA_SUCCESS;
    }
  } else {
    // неупорядоченный индекс (ключи всегда хешируются)
    if (unlikely(!fpta_index_unordered_is_compat(type, value.type)))
      return FPTA_ETYPE;

    if (value.type == fpta_shoved) {
      if (unlikely(value.binary_length != sizeof(key.place.u64)))
        return FPTA_DATALEN_MISMATCH;
      if (unlikely(value.binary_data == nullptr))
        return FPTA_EINVAL;

      key.mdbx.mv_size = sizeof(key.place.u64);
      key.mdbx.mv_data = value.binary_data;
      if (copy) {
        memcpy(&key.place, key.mdbx.mv_data, sizeof(key.place.u64));
        key.mdbx.mv_data = &key.place;
      }
      return FPTA_SUCCESS;
    }
  }

  switch (type) {
  case fptu_nested:
    // TODO: додумать как лучше преобразовывать кортеж в ключ.
    return FPTA_ENOIMP;

  default:
  /* TODO: проверить корректность размера для fptu_farray */
  case fptu_opaque:
    /* не позволяем смешивать string и opaque/binary, в том числе
     * чтобы избежать путаницы между строками в utf8 и unicode,
     * а также прочих последствий излишней гибкости. */
    assert(value.type != fpta_string);
    if (unlikely(value.type == fpta_string))
      return FPTA_EOOPS;
    if (unlikely(value.binary_data == nullptr) && value.binary_length)
      return FPTA_EINVAL;
    key.mdbx.mv_size = value.binary_length;
    key.mdbx.mv_data = value.binary_data;
    break;

  case fptu_null:
    return FPTA_EOOPS;

  case fptu_uint16:
    key.place.u32 = (uint16_t)value.sint;
    if (unlikely(value.sint != key.place.u32))
      return FPTA_EVALUE;
    key.mdbx.mv_size = sizeof(key.place.u32);
    key.mdbx.mv_data = &key.place.u32;
    return FPTA_SUCCESS;

  case fptu_uint32:
    key.place.u32 = (uint32_t)value.sint;
    if (unlikely(value.sint != key.place.u32))
      return FPTA_EVALUE;
    key.mdbx.mv_size = sizeof(key.place.u32);
    key.mdbx.mv_data = &key.place.u32;
    return FPTA_SUCCESS;

  case fptu_int32:
    key.place.i32 = (int32_t)value.sint;
    if (unlikely(value.sint != key.place.i32))
      return FPTA_EVALUE;
    key.mdbx.mv_size = sizeof(key.place.i32);
    key.mdbx.mv_data = &key.place.i32;
    return FPTA_SUCCESS;

  case fptu_fp32:
    key.mdbx.mv_size = sizeof(key.place.f32);
    key.mdbx.mv_data = &key.place.f32;
    key.place.f32 = (float)value.fp;
    switch (std::fpclassify(key.place.f32)) {
    case FP_INFINITE:
      if (std::isinf(value.fp))
        break;
    case FP_NAN:
    default:
      return FPTA_EVALUE;
    case FP_SUBNORMAL:
    case FP_ZERO:
      key.place.f32 = 0;
    case FP_NORMAL:
      break;
    }
#if FPTA_PROHIBIT_LOSS_PRECISION
    if (unlikely(value.fp != key.place.f32))
      return FPTA_EVALUE;
#endif
    return FPTA_SUCCESS;

  case fptu_int64:
    if (unlikely(value.type == fpta_unsigned_int && value.uint > INT64_MAX))
      return FPTA_EVALUE;
    key.place.i64 = value.sint;
    key.mdbx.mv_size = sizeof(key.place.i64);
    key.mdbx.mv_data = &key.place.i64;
    return FPTA_SUCCESS;

  case fptu_uint64:
    if (unlikely(value.type == fpta_signed_int && value.sint < 0))
      return FPTA_EVALUE;
    key.place.u64 = value.uint;
    key.mdbx.mv_size = sizeof(key.place.u64);
    key.mdbx.mv_data = &key.place.u64;
    return FPTA_SUCCESS;

  case fptu_fp64:
    key.mdbx.mv_size = sizeof(key.place.f64);
    key.mdbx.mv_data = &key.place.f64;
    key.place.f64 = value.fp;
    switch (std::fpclassify(key.place.f64)) {
    case FP_NAN:
    default:
      return FPTA_EVALUE;
    case FP_SUBNORMAL:
    case FP_ZERO:
      key.place.f64 = 0;
    case FP_INFINITE:
    case FP_NORMAL:
      break;
    }
    return FPTA_SUCCESS;

  case fptu_datetime:
    assert(value.type == fpta_datetime);
    key.place.u64 = value.uint;
    key.mdbx.mv_size = sizeof(key.place.u64);
    key.mdbx.mv_data = &key.place.u64;
    return FPTA_SUCCESS;

  case fptu_cstr:
    /* не позволяем смешивать string и opaque/binary, в том числе
     * чтобы избежать путаницы между строками в utf8 и unicode,
     * а также прочих последствий излишней гибкости. */
    assert(value.type == fpta_string);
    if (unlikely(value.type != fpta_string))
      return FPTA_EOOPS;
    if (unlikely(value.str == nullptr) && value.binary_length)
      return FPTA_EINVAL;
    key.mdbx.mv_size = value.binary_length;
    key.mdbx.mv_data = (void *)value.str;
    assert(strnlen(value.str, key.mdbx.mv_size) == key.mdbx.mv_size);
    break;

  case fptu_96:
    key.mdbx.mv_size = value.binary_length;
    key.mdbx.mv_data = value.binary_data;
    if (unlikely(value.binary_length != 96 / 8))
      return FPTA_DATALEN_MISMATCH;
    break;

  case fptu_128:
    key.mdbx.mv_size = value.binary_length;
    key.mdbx.mv_data = value.binary_data;
    if (unlikely(value.binary_length != 128 / 8))
      return FPTA_DATALEN_MISMATCH;
    break;

  case fptu_160:
    key.mdbx.mv_size = value.binary_length;
    key.mdbx.mv_data = value.binary_data;
    if (unlikely(value.binary_length != 160 / 8))
      return FPTA_DATALEN_MISMATCH;
    break;

  case fptu_256:
    key.mdbx.mv_size = value.binary_length;
    key.mdbx.mv_data = value.binary_data;
    if (unlikely(value.binary_length != 256 / 8))
      return FPTA_DATALEN_MISMATCH;
    break;
  }

  return fpta_normalize_key(shove, key, copy);
}

int __fpta_index_value2key(fpta_shove_t shove, const fpta_value *value,
                           void *key) {
  return fpta_index_value2key(shove, *value, *(fpta_key *)key, true);
}

//----------------------------------------------------------------------------

int fpta_index_key2value(fpta_shove_t shove, const MDB_val &mdbx,
                         fpta_value &value) {
  fptu_type type = fpta_shove2type(shove);
  fpta_index_type index = fpta_shove2index(shove);

  if (type > fptu_fp64 && !fpta_index_is_ordered(index)) {
    if (unlikely(mdbx.mv_size != sizeof(uint64_t)))
      return FPTA_INDEX_CORRUPTED;

    value.uint = *(uint64_t *)mdbx.mv_data;
    value.binary_data = &value.uint;
    value.binary_length = sizeof(uint64_t);
    value.type = fpta_shoved;
    return FPTA_SUCCESS;
  }

  switch (type) {
  case fptu_null:
    value.type = fpta_null;
    value.binary_data = nullptr;
    value.binary_length = 0;
    return FPTA_EOOPS;

  default:
  /* TODO: проверить корректность размера для fptu_farray */
  case fptu_nested:
    if (unlikely(mdbx.mv_size % sizeof(fptu_unit)))
      return FPTA_INDEX_CORRUPTED;
  case fptu_opaque:
    value.type = (mdbx.mv_size > fpta_max_keylen) ? fpta_shoved : fpta_binary;
    value.binary_data = mdbx.mv_data;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;

  case fptu_cstr:
    value.type = (mdbx.mv_size > fpta_max_keylen) ? fpta_shoved : fpta_string;
    value.binary_data = mdbx.mv_data;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;

  case fptu_uint16: {
    if (unlikely(mdbx.mv_size != sizeof(uint32_t)))
      return FPTA_INDEX_CORRUPTED;
    auto tmp = *(uint32_t *)mdbx.mv_data;
    if (tmp != (uint16_t)tmp)
      return FPTA_INDEX_CORRUPTED;
    value.type = fpta_unsigned_int;
    value.uint = tmp;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;
  }

  case fptu_uint32: {
    if (unlikely(mdbx.mv_size != sizeof(uint32_t)))
      return FPTA_INDEX_CORRUPTED;
    value.type = fpta_unsigned_int;
    value.uint = *(uint32_t *)mdbx.mv_data;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;
  }

  case fptu_int32: {
    if (unlikely(mdbx.mv_size != sizeof(int32_t)))
      return FPTA_INDEX_CORRUPTED;
    value.type = fpta_signed_int;
    value.sint = *(int32_t *)mdbx.mv_data;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;
  }

  case fptu_fp32: {
    if (unlikely(mdbx.mv_size != sizeof(float)))
      return FPTA_INDEX_CORRUPTED;
    value.type = fpta_float_point;
    value.fp = *(float *)mdbx.mv_data;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;
  }

  case fptu_fp64: {
    if (unlikely(mdbx.mv_size != sizeof(double)))
      return FPTA_INDEX_CORRUPTED;
    value.type = fpta_float_point;
    value.fp = *(double *)mdbx.mv_data;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;
  }

  case fptu_uint64: {
    if (unlikely(mdbx.mv_size != sizeof(uint64_t)))
      return FPTA_INDEX_CORRUPTED;
    value.type = fpta_unsigned_int;
    value.uint = *(uint64_t *)mdbx.mv_data;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;
  }

  case fptu_int64: {
    if (unlikely(mdbx.mv_size != sizeof(int64_t)))
      return FPTA_INDEX_CORRUPTED;
    value.type = fpta_signed_int;
    value.sint = *(int64_t *)mdbx.mv_data;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;
  }

  case fptu_datetime: {
    if (unlikely(mdbx.mv_size != sizeof(uint64_t)))
      return FPTA_INDEX_CORRUPTED;
    value.type = fpta_datetime;
    value.datetime.fixedpoint = *(uint64_t *)mdbx.mv_data;
    value.binary_length = (unsigned)mdbx.mv_size;
    return FPTA_SUCCESS;
  }

  case fptu_96:
    if (unlikely(mdbx.mv_size != 96 / 8))
      return FPTA_INDEX_CORRUPTED;
    break;

  case fptu_128:
    if (unlikely(mdbx.mv_size != 128 / 8))
      return FPTA_INDEX_CORRUPTED;
    break;

  case fptu_160:
    if (unlikely(mdbx.mv_size != 160 / 8))
      return FPTA_INDEX_CORRUPTED;
    break;

  case fptu_256:
    if (unlikely(mdbx.mv_size != 256 / 8))
      return FPTA_INDEX_CORRUPTED;
    break;
  }

  value.type = fpta_binary;
  value.binary_data = mdbx.mv_data;
  value.binary_length = (unsigned)mdbx.mv_size;
  return FPTA_SUCCESS;
}

//----------------------------------------------------------------------------

__hot int fpta_index_row2key(fpta_shove_t shove, size_t column,
                             const fptu_ro &row, fpta_key &key, bool copy) {
#ifndef NDEBUG
  fpta_pollute(&key, sizeof(key), 0);
#endif

  fptu_type type = fpta_shove2type(shove);
  const fptu_field *field = fptu_lookup_ro(row, (unsigned)column, type);

  if (unlikely(field == nullptr))
    return FPTA_COLUMN_MISSING;

  const fptu_payload *payload = fptu_field_payload(field);
  switch (type) {
  case fptu_nested:
    // TODO: додумать как лучше преобразовывать кортеж в ключ.
    return FPTA_ENOIMP;

  default:
    /* TODO: проверить корректность размера для fptu_farray */
    key.mdbx.mv_size = units2bytes(payload->other.varlen.brutto);
    key.mdbx.mv_data = (void *)payload->other.data;
    break;

  case fptu_opaque:
    key.mdbx.mv_size = payload->other.varlen.opaque_bytes;
    key.mdbx.mv_data = (void *)payload->other.data;
    break;

  case fptu_null:
    return FPTA_EOOPS;

  case fptu_uint16:
    key.place.u32 = field->get_payload_uint16();
    key.mdbx.mv_size = sizeof(key.place.u32);
    key.mdbx.mv_data = &key.place.u32;
    return FPTA_SUCCESS;

  case fptu_fp32:
  /*if (unlikely(std::isnan(payload->fp32)))
      return FPTA_EVALUE;*/
  case fptu_int32:
  case fptu_uint32:
    static_assert(sizeof(key.place.f32) == sizeof(key.place.i32),
                  "something wrong");
    static_assert(sizeof(key.place.i32) == sizeof(key.place.u32),
                  "something wrong");
    key.place.u32 = payload->u32;
    key.mdbx.mv_size = sizeof(key.place.u32);
    key.mdbx.mv_data = &key.place.u32;
    return FPTA_SUCCESS;

  case fptu_fp64:
  /*if (unlikely(std::isnan(payload->fp64)))
      return FPTA_EVALUE;*/
  case fptu_int64:
  case fptu_uint64:
  case fptu_datetime:
    static_assert(sizeof(key.place.f64) == sizeof(key.place.i64),
                  "something wrong");
    static_assert(sizeof(key.place.i64) == sizeof(key.place.u64),
                  "something wrong");
    key.place.u64 = payload->u64;
    key.mdbx.mv_size = sizeof(key.place.u64);
    key.mdbx.mv_data = &key.place.u64;
    return FPTA_SUCCESS;

  case fptu_cstr:
    key.mdbx.mv_data = (void *)payload->cstr;
    key.mdbx.mv_size = strlen(payload->cstr);
    break;

  case fptu_96:
    key.mdbx.mv_size = 96 / 8;
    key.mdbx.mv_data = (void *)payload->fixbin;
    break;

  case fptu_128:
    key.mdbx.mv_size = 128 / 8;
    key.mdbx.mv_data = (void *)payload->fixbin;
    break;

  case fptu_160:
    key.mdbx.mv_size = 160 / 8;
    key.mdbx.mv_data = (void *)payload->fixbin;
    break;

  case fptu_256:
    key.mdbx.mv_size = 256 / 8;
    key.mdbx.mv_data = (void *)payload->fixbin;
    break;
  }

  return fpta_normalize_key(shove, key, copy);
}
