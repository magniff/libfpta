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

/* Подставляется в качестве адреса для ключей нулевой длины,
 * с тем чтобы отличать от nullptr */
static char NIL;

bool fpta_cursor_validate(const fpta_cursor *cursor, fpta_level min_level) {
  if (unlikely(cursor == nullptr || cursor->mdbx_cursor == nullptr ||
               !fpta_txn_validate(cursor->txn, min_level)))
    return false;

  // TODO
  return true;
}

int fpta_cursor_close(fpta_cursor *cursor) {
  if (unlikely(!fpta_cursor_validate(cursor, fpta_read)))
    return FPTA_EINVAL;

  mdbx_cursor_close(cursor->mdbx_cursor);
  fpta_cursor_free(cursor->db, cursor);
  return FPTA_SUCCESS;
}

int fpta_cursor_open(fpta_txn *txn, fpta_name *column_id, fpta_value range_from,
                     fpta_value range_to, const fpta_filter *filter,
                     fpta_cursor_options op, fpta_cursor **pcursor) {
  if (unlikely(pcursor == nullptr))
    return FPTA_EINVAL;
  *pcursor = nullptr;

  switch (op) {
  default:
    return FPTA_EINVAL;

  case fpta_descending:
  case fpta_descending_dont_fetch:
  case fpta_unsorted:
  case fpta_unsorted_dont_fetch:
  case fpta_ascending:
  case fpta_ascending_dont_fetch:
    break;
  }

  if (unlikely(!fpta_id_validate(column_id, fpta_column)))
    return FPTA_EINVAL;

  fpta_name *table_id = column_id->column.table;
  int rc = fpta_name_refresh_couple(txn, table_id, column_id);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  fpta_index_type index = fpta_shove2index(column_id->shove);
  if (unlikely(index == fpta_index_none))
    return FPTA_NO_INDEX;

  if (!fpta_index_is_ordered(index) && fpta_cursor_is_ordered(op))
    return FPTA_NO_INDEX;

  if (unlikely(!fpta_index_is_compat(column_id->shove, range_from) ||
               !fpta_index_is_compat(column_id->shove, range_to)))
    return FPTA_ETYPE;

  if (unlikely(range_from.type == fpta_end || range_to.type == fpta_begin))
    return FPTA_EINVAL;

  if (unlikely(!fpta_filter_validate(filter)))
    return FPTA_EINVAL;

  if (unlikely(column_id->mdbx_dbi < 1)) {
    rc = fpta_open_column(txn, column_id);
    if (unlikely(rc != FPTA_SUCCESS))
      return rc;
  }

  fpta_db *db = txn->db;
  fpta_cursor *cursor = fpta_cursor_alloc(db);
  if (unlikely(cursor == nullptr))
    return FPTA_ENOMEM;

  cursor->options = op;
  cursor->txn = txn;
  cursor->filter = filter;
  cursor->table_id = table_id;
  cursor->index.shove =
      column_id->shove & (fpta_column_typeid_mask | fpta_column_index_mask);
  cursor->index.column_order = (unsigned)column_id->column.num;
  cursor->index.mdbx_dbi = column_id->mdbx_dbi;

  if (range_from.type != fpta_begin) {
    rc = fpta_index_value2key(cursor->index.shove, range_from,
                              cursor->range_from_key, true);
    if (unlikely(rc != FPTA_SUCCESS))
      goto bailout;
    assert(cursor->range_from_key.mdbx.iov_base != nullptr);
  }

  if (range_to.type != fpta_end) {
    rc = fpta_index_value2key(cursor->index.shove, range_to,
                              cursor->range_to_key, true);
    if (unlikely(rc != FPTA_SUCCESS))
      goto bailout;
    assert(cursor->range_to_key.mdbx.iov_base != nullptr);
  }

  rc = mdbx_cursor_open(txn->mdbx_txn, cursor->index.mdbx_dbi,
                        &cursor->mdbx_cursor);
  if (unlikely(rc != MDB_SUCCESS))
    goto bailout;

  if ((op & fpta_dont_fetch) == 0) {
    rc = fpta_cursor_move(cursor, fpta_first);
    if (unlikely(rc != MDB_SUCCESS))
      goto bailout;
  }

  *pcursor = cursor;
  return FPTA_SUCCESS;

bailout:
  if (cursor->mdbx_cursor)
    mdbx_cursor_close(cursor->mdbx_cursor);
  fpta_cursor_free(db, cursor);
  return rc;
}

//----------------------------------------------------------------------------

static int fpta_cursor_seek(fpta_cursor *cursor, MDB_cursor_op mdbx_seek_op,
                            MDB_cursor_op mdbx_step_op,
                            const MDB_val *mdbx_seek_key,
                            const MDB_val *mdbx_seek_data) {
  assert(mdbx_seek_key != &cursor->current);
  fptu_ro mdbx_data;
  int rc;

  if (likely(mdbx_seek_key == NULL)) {
    assert(mdbx_seek_data == NULL);
    rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, &mdbx_data.sys,
                         mdbx_seek_op);
  } else {
    /* Помещаем целевой ключ и данные (адреса и размер)
     * в cursor->current и mdbx_data, это требуется для того чтобы:
     *   - после возврата из mdbx_cursor_get() в cursor->current и mdbx_data
     *     уже был указатели на ключ и данные в БД, без необходимости
     *     еще одного вызова mdbx_cursor_get(MDB_GET_CURRENT).
     *   - если передать непосредственно mdbx_seek_key и mdbx_seek_data,
     *     то исходные значения будут потеряны (перезаписаны), что создаст
     *     сложности при последующей корректировке позиции. Например, для
     *     перемещения за lower_bound для descending в fpta_cursor_locate().
     */
    cursor->current.iov_len = mdbx_seek_key->iov_len;
    cursor->current.iov_base =
        /* Замещаем nullptr для ключей нулевой длинны, так чтобы
         * в курсоре стоящем на строке с ключом нулевой длины
         * cursor->current.iov_base != nullptr, и тем самым курсор
         * не попадал под критерий is_poor() */
        mdbx_seek_key->iov_base ? mdbx_seek_key->iov_base : &NIL;

    if (!mdbx_seek_data)
      rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, nullptr,
                           mdbx_seek_op);
    else {
      mdbx_data.sys = *mdbx_seek_data;
      rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current,
                           &mdbx_data.sys, mdbx_seek_op);
      if (likely(rc == MDB_SUCCESS))
        rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current,
                             &mdbx_data.sys, MDB_GET_CURRENT);
    }

    if (rc == MDB_SUCCESS) {
      assert(cursor->current.iov_base != mdbx_seek_key->iov_base);
      if (mdbx_seek_data)
        assert(mdbx_data.sys.iov_base != mdbx_seek_data->iov_base);
    }

    if (fpta_cursor_is_descending(cursor->options) &&
        (mdbx_seek_op == MDB_GET_BOTH_RANGE || mdbx_seek_op == MDB_SET_RANGE)) {
      /* Корректировка перемещения для курсора с сортировкой по-убыванию.
       *
       * Внутри mdbx_cursor_get() выполняет позиционирование аналогично
       * std::lower_bound() при сортировке по-возрастанию. Поэтому при
       * поиске для курсора с сортировкой в обратном порядке необходимо
       * выполнить махинации:
       *  - Если ключ в фактически самой последней строке оказался меньше
       *    искомого, то при результате MDB_NOTFOUND от mdbx_cursor_get()
       *    следует к последней строке, что будет соответствовать переходу
       *    к самой первой позиции при обратной сортировке.
       *  - Если искомый ключ не найден и курсор стоит на фактически самой
       *    первой строке, то следует вернуть результат "нет данных", что
       *    будет соответствовать поведению lower_bound при сортировке
       *    в обратном порядке.
       *  - Если искомый ключ найден, то перейти к "первой" равной строке
       *    в порядке курсора, что означает перейти к последнему дубликату.
       *    По эстетическим соображениям этот переход реализован не здесь,
       *    а в fpta_cursor_locate().
       */
      if (rc == MDB_SUCCESS &&
          mdbx_cursor_on_first(cursor->mdbx_cursor) == MDBX_RESULT_TRUE &&
          mdbx_cmp(cursor->txn->mdbx_txn, cursor->index.mdbx_dbi,
                   &cursor->current, mdbx_seek_key) < 0) {
        goto eof;
      } else if (rc == MDB_NOTFOUND &&
                 mdbx_cursor_on_last(cursor->mdbx_cursor) == MDBX_RESULT_TRUE) {
        rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current,
                             &mdbx_data.sys, MDB_LAST);
      }
    }
  }

  while (rc == MDB_SUCCESS) {
    MDB_cursor_op step_op = mdbx_step_op;

    if (cursor->range_from_key.mdbx.iov_base &&
        mdbx_cmp(cursor->txn->mdbx_txn, cursor->index.mdbx_dbi,
                 &cursor->current, &cursor->range_from_key.mdbx) < 0) {
      /* задана нижняя граница диапазона и текущий ключ меньше её */
      switch (step_op) {
      default:
        assert(false);
      case MDB_PREV_DUP:
      case MDB_NEXT_DUP:
        /* нет смысла идти по дубликатам (без изменения значения ключа) */
        break;
      case MDB_PREV:
        step_op = MDB_PREV_NODUP;
      case MDB_PREV_NODUP:
        /* идти в сторону уменьшения ключа есть смысл только в случае
         * unordered (хэшированного) индекса, при этом логично пропустить
         * все дубликаты, так как они заведомо не попадают в диапазон курсора */
        if (!fpta_index_is_ordered(cursor->index.shove))
          goto next;
        break;
      case MDB_NEXT:
        /* при движении в сторону увеличения ключа логично пропустить все
         * дубликаты, так как они заведомо не попадают в диапазон курсора */
        step_op = MDB_NEXT_NODUP;
      case MDB_NEXT_NODUP:
        goto next;
      }
      goto eof;
    }

    if (cursor->range_to_key.mdbx.iov_base &&
        mdbx_cmp(cursor->txn->mdbx_txn, cursor->index.mdbx_dbi,
                 &cursor->current, &cursor->range_to_key.mdbx) >= 0) {
      /* задана верхняя граница диапазона и текущий ключ больше её */
      switch (step_op) {
      default:
        assert(false);
      case MDB_PREV_DUP:
      case MDB_NEXT_DUP:
        /* нет смысла идти по дубликатам (без изменения значения ключа) */
        break;
      case MDB_PREV:
        /* при движении в сторону уменьшения ключа логично пропустить все
         * дубликаты, так как они заведомо не попадают в диапазон курсора */
        step_op = MDB_PREV_NODUP;
      case MDB_PREV_NODUP:
        goto next;
      case MDB_NEXT:
        step_op = MDB_NEXT_NODUP;
      case MDB_NEXT_NODUP:
        /* идти в сторону увелияения ключа есть смысл только в случае
         * unordered (хэшированного) индекса, при этом логично пропустить
         * все дубликаты, так как они заведомо не попадают в диапазон курсора */
        if (!fpta_index_is_ordered(cursor->index.shove))
          goto next;
        break;
      }
      goto eof;
    }

    if (!cursor->filter)
      return FPTA_SUCCESS;

    if (fpta_index_is_secondary(cursor->index.shove)) {
      MDB_val pk_key = mdbx_data.sys;
      rc = mdbx_get(cursor->txn->mdbx_txn, cursor->table_id->mdbx_dbi, &pk_key,
                    &mdbx_data.sys);
      if (unlikely(rc != MDB_SUCCESS))
        return (rc != MDB_NOTFOUND) ? rc : (int)FPTA_INDEX_CORRUPTED;
    }

    if (fpta_filter_match(cursor->filter, mdbx_data))
      return FPTA_SUCCESS;

  next:
    rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, &mdbx_data.sys,
                         step_op);
  }

  if (unlikely(rc != MDB_NOTFOUND)) {
    cursor->set_poor();
    return rc;
  }

eof:
  switch (mdbx_seek_op) {
  default:
    cursor->set_poor();
    return FPTA_NODATA;

  case MDB_NEXT:
  case MDB_NEXT_NODUP:
    cursor->set_eof(fpta_cursor::after_last);
    return FPTA_NODATA;

  case MDB_PREV:
  case MDB_PREV_NODUP:
    cursor->set_eof(fpta_cursor::before_first);
    return FPTA_NODATA;

  case MDB_PREV_DUP:
  case MDB_NEXT_DUP:
    return FPTA_NODATA;
  }
}

int fpta_cursor_move(fpta_cursor *cursor, fpta_seek_operations op) {
  if (unlikely(!fpta_cursor_validate(cursor, fpta_read)))
    return FPTA_EINVAL;

  if (unlikely(op < fpta_first || op > fpta_key_prev)) {
    cursor->set_poor();
    return FPTA_EINVAL;
  }

  if (fpta_cursor_is_descending(cursor->options))
    op = (fpta_seek_operations)(op ^ 1);

  MDB_val *mdbx_seek_key = nullptr;
  MDB_cursor_op mdbx_seek_op, mdbx_step_op;
  switch (op) {
  default:
    assert(false && "unexpected seek-op");
    cursor->set_poor();
    return FPTA_EOOPS;

  case fpta_first:
    if (cursor->range_from_key.mdbx.iov_base == nullptr ||
        !fpta_index_is_ordered(cursor->index.shove)) {
      mdbx_seek_op = MDB_FIRST;
    } else {
      mdbx_seek_key = &cursor->range_from_key.mdbx;
      mdbx_seek_op = MDB_SET_RANGE;
    }
    mdbx_step_op = MDB_NEXT;
    break;

  case fpta_last:
    if (cursor->range_to_key.mdbx.iov_base == nullptr ||
        !fpta_index_is_ordered(cursor->index.shove)) {
      mdbx_seek_op = MDB_LAST;
    } else {
      mdbx_seek_key = &cursor->range_to_key.mdbx;
      mdbx_seek_op = MDB_SET_RANGE;
    }
    mdbx_step_op = MDB_PREV;
    break;

  case fpta_next:
    if (unlikely(cursor->is_poor()))
      return FPTA_ECURSOR;
    mdbx_seek_op = unlikely(cursor->is_before_first()) ? MDB_FIRST : MDB_NEXT;
    mdbx_step_op = MDB_NEXT;
    break;
  case fpta_prev:
    if (unlikely(cursor->is_poor()))
      return FPTA_ECURSOR;
    mdbx_seek_op = unlikely(cursor->is_after_last()) ? MDB_LAST : MDB_PREV;
    mdbx_step_op = MDB_PREV;
    break;

  /* Перемещение по дубликатам значения ключа, в случае если
   * соответствующий индекс был БЕЗ флага fpta_index_uniq */
  case fpta_dup_first:
    if (unlikely(!cursor->is_filled()))
      return cursor->unladed_state();
    if (unlikely(fpta_index_is_unique(cursor->index.shove)))
      return FPTA_SUCCESS;
    mdbx_seek_op = MDB_FIRST_DUP;
    mdbx_step_op = MDB_NEXT_DUP;
    break;

  case fpta_dup_last:
    if (unlikely(!cursor->is_filled()))
      return cursor->unladed_state();
    if (unlikely(fpta_index_is_unique(cursor->index.shove)))
      return FPTA_SUCCESS;
    mdbx_seek_op = MDB_LAST_DUP;
    mdbx_step_op = MDB_PREV_DUP;
    break;

  case fpta_dup_next:
    if (unlikely(!cursor->is_filled()))
      return cursor->unladed_state();
    if (unlikely(fpta_index_is_unique(cursor->index.shove)))
      return FPTA_NODATA;
    mdbx_seek_op = MDB_NEXT_DUP;
    mdbx_step_op = MDB_NEXT_DUP;
    break;

  case fpta_dup_prev:
    if (unlikely(!cursor->is_filled()))
      return cursor->unladed_state();
    if (unlikely(fpta_index_is_unique(cursor->index.shove)))
      return FPTA_NODATA;
    mdbx_seek_op = MDB_PREV_DUP;
    mdbx_step_op = MDB_PREV_DUP;
    break;

  case fpta_key_next:
    if (unlikely(cursor->is_poor()))
      return FPTA_ECURSOR;
    mdbx_seek_op =
        unlikely(cursor->is_before_first()) ? MDB_FIRST : MDB_NEXT_NODUP;
    mdbx_step_op = MDB_NEXT_NODUP;
    break;

  case fpta_key_prev:
    if (unlikely(cursor->is_poor()))
      return FPTA_ECURSOR;
    mdbx_seek_op =
        unlikely(cursor->is_after_last()) ? MDB_LAST : MDB_PREV_NODUP;
    mdbx_step_op = MDB_PREV_NODUP;
    break;
  }

  return fpta_cursor_seek(cursor, mdbx_seek_op, mdbx_step_op, mdbx_seek_key,
                          nullptr);
}

int fpta_cursor_locate(fpta_cursor *cursor, bool exactly, const fpta_value *key,
                       const fptu_ro *row) {
  if (unlikely(!fpta_cursor_validate(cursor, fpta_read)))
    return FPTA_EINVAL;

  if (unlikely((key && row) || (!key && !row))) {
    /* Должен быть выбран один из режимов поиска. */
    cursor->set_poor();
    return FPTA_EINVAL;
  }

  if (!fpta_cursor_is_ordered(cursor->options)) {
    if (FPTA_PROHIBIT_NEARBY4UNORDERED && !exactly) {
      /* Отвергаем неточный поиск для неупорядоченного курсора (и индекса). */
      cursor->set_poor();
      return FPTA_EINVAL;
    }
    /* Принудительно включаем точный поиск для курсора без сортировки. */
    exactly = true;
  }

  /* устанавливаем базовый режим поиска */
  MDB_cursor_op mdbx_seek_op = exactly ? MDB_SET_KEY : MDB_SET_RANGE;
  const MDB_val *mdbx_seek_data = nullptr;

  fpta_key seek_key, pk_key;
  int rc;
  if (key) {
    /* Поиск по значению проиндексированной колонки, конвертируем его в ключ
     * для поиска по индексу. Дополнительных данных для поиска нет. */
    rc = fpta_index_value2key(cursor->index.shove, *key, seek_key, false);
    if (unlikely(rc != FPTA_SUCCESS)) {
      cursor->set_poor();
      return rc;
    }
    /* базовый режим поиска уже выставлен. */
  } else {
    /* Поиск по "образу" строки, получаем из строки-кортежа значение
     * проиндексированной колонки в формате ключа для поиска по индексу. */
    rc = fpta_index_row2key(cursor->index.shove, cursor->index.column_order,
                            *row, seek_key, false);
    if (unlikely(rc != FPTA_SUCCESS)) {
      cursor->set_poor();
      return rc;
    }

    if (fpta_index_is_secondary(cursor->index.shove)) {
      /* Курсор связан со вторичным индексом. Для уточнения поиска можем
       * использовать только значение PK. */
      if (fpta_index_is_unique(cursor->index.shove)) {
        /* Не используем PK если вторичный индекс обеспечивает уникальность.
         * Базовый режим поиска уже был выставлен. */
      } else {
        /* Извлекаем и используем значение PK только если связанный с
         * курсором индекс допускает дубликаты. */
        rc = fpta_index_row2key(cursor->table_id->table.pk, 0, *row, pk_key,
                                false);
        if (rc == FPTA_SUCCESS) {
          /* Используем уточняющее значение PK только если в строке-образце
           * есть соответствующая колонка. При этом игнорируем отсутствие
           * колонки (ошибку FPTA_COLUMN_MISSING). */
          mdbx_seek_data = &pk_key.mdbx;
          mdbx_seek_op = exactly ? MDB_GET_BOTH : MDB_GET_BOTH_RANGE;
        } else if (rc != FPTA_COLUMN_MISSING) {
          cursor->set_poor();
          return rc;
        } else {
          /* в строке нет колонки с PK, базовый режим поиска уже выставлен. */
        }
      }
    } else {
      /* Курсор связан с первичным индексом. Для уточнения поиска можем
       * использовать только данные (значение) всей строки. Однако,
       * делаем это ТОЛЬКО при неточном поиске по индексу с дубликатами,
       * так как только в этом случае это выглядит рациональным:
       *  - При точном поиске, отличие в значении любой колонки, включая
       *    её отсутствие, даст отрицательный результат.
       *    Соответственно, это породит кардинальные отличия от поведения
       *    в других лучаях. Например, когда используется вторичный индекс.
       *  - Фактически будет выполнятется не позиционирование курсора, а
       *    некая комплекстная операция "найти заданную строку таблицы",
       *    полезность которой сомнительна. */
      if (!exactly && !fpta_index_is_unique(cursor->index.shove)) {
        /* базовый режим поиска уже был выставлен, переключаем только
         * для нечеткого поиска среди дубликатов (как описано выше). */
        mdbx_seek_data = &row->sys;
        mdbx_seek_op = MDB_GET_BOTH_RANGE;
      }
    }
  }

  rc = fpta_cursor_seek(cursor, mdbx_seek_op,
                        fpta_cursor_is_descending(cursor->options) ? MDB_PREV
                                                                   : MDB_NEXT,
                        &seek_key.mdbx, mdbx_seek_data);
  if (unlikely(rc != FPTA_SUCCESS)) {
    cursor->set_poor();
    return rc;
  }

  if (!fpta_cursor_is_descending(cursor->options))
    return FPTA_SUCCESS;

  /* Корректируем позицию при обратном порядке строк (fpta_descending) */
  while (!exactly) {
    /* При неточном поиске для курсора с обратной сортировкой нужно перейти
     * на другую сторону от lower_bound, т.е. идти в обратном порядке
     * до значения меньшего или равного целевому (с учетом фильтра). */
    int cmp = mdbx_cmp(cursor->txn->mdbx_txn, cursor->index.mdbx_dbi,
                       &cursor->current, &seek_key.mdbx);

    if (cmp < 0)
      return FPTA_SUCCESS;

    if (cmp == 0) {
      if (!mdbx_seek_data) {
        /* Поиск без уточнения по дубликатам. Если индекс допускает
         * дубликаты, то следует перейти к последнему, что будет
         * сделао ниже. */
        break;
      }

      /* Неточный поиск с уточнением по дубликатам. Переход на другую
       * сторону lower_bound следует делать с учетом сравнения данных. */
      MDB_val mdbx_data;
      rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, &mdbx_data,
                           MDB_GET_CURRENT);
      if (unlikely(rc != FPTA_SUCCESS)) {
        cursor->set_poor();
        return rc;
      }

      cmp = mdbx_dcmp(cursor->txn->mdbx_txn, cursor->index.mdbx_dbi, &mdbx_data,
                      mdbx_seek_data);
      if (cmp <= 0)
        return FPTA_SUCCESS;
    }

    rc = fpta_cursor_seek(cursor, MDB_PREV, MDB_PREV, nullptr, nullptr);
    if (unlikely(rc != FPTA_SUCCESS)) {
      cursor->set_poor();
      return rc;
    }
  }

  /* Для индекса с дубликатами нужно перейти к последней позиции с текущим
   * ключом. */
  if (!fpta_index_is_unique(cursor->index.shove)) {
    size_t dups;
    if (unlikely(mdbx_cursor_count(cursor->mdbx_cursor, &dups) !=
                 MDB_SUCCESS)) {
      cursor->set_poor();
      return FPTA_EOOPS;
    }

    if (dups > 1) {
      /* Переходим к последнему дубликату (последнему мульти-значению
       * для одного значения ключа), а если значение не подходит под
       * фильтр, то двигаемся в обратном порядке дальше. */
      rc = fpta_cursor_seek(cursor, MDB_LAST_DUP, MDB_PREV, nullptr, nullptr);
      if (unlikely(rc != FPTA_SUCCESS)) {
        cursor->set_poor();
        return rc;
      }
    }
  }

  return FPTA_SUCCESS;
}

//----------------------------------------------------------------------------

int fpta_cursor_eof(fpta_cursor *cursor) {
  if (unlikely(!fpta_cursor_validate(cursor, fpta_read)))
    return FPTA_EINVAL;

  if (likely(cursor->is_filled()))
    return FPTA_SUCCESS;

  return FPTA_NODATA;
}

int fpta_cursor_count(fpta_cursor *cursor, size_t *pcount, size_t limit) {
  if (unlikely(!pcount))
    return FPTA_EINVAL;
  *pcount = (size_t)FPTA_DEADBEEF;

  size_t count = 0;
  int rc = fpta_cursor_move(cursor, fpta_first);
  while (rc == FPTA_SUCCESS && count < limit) {
    ++count;
    rc = fpta_cursor_move(cursor, fpta_next);
  }

  if (rc == FPTA_NODATA) {
    *pcount = count;
    rc = FPTA_SUCCESS;
  }

  cursor->set_poor();
  return rc;
}

int fpta_cursor_dups(fpta_cursor *cursor, size_t *pdups) {
  if (unlikely(pdups == nullptr))
    return FPTA_EINVAL;
  *pdups = (size_t)FPTA_DEADBEEF;

  if (unlikely(!fpta_cursor_validate(cursor, fpta_read)))
    return FPTA_EINVAL;

  if (unlikely(!cursor->is_filled())) {
    if (cursor->is_poor())
      return FPTA_ECURSOR;
    *pdups = 0;
    return FPTA_NODATA;
  }

  *pdups = 0;
  int rc = mdbx_cursor_count(cursor->mdbx_cursor, pdups);
  return (rc == MDB_NOTFOUND) ? (int)FPTA_NODATA : rc;
}

//----------------------------------------------------------------------------

int fpta_cursor_get(fpta_cursor *cursor, fptu_ro *row) {
  if (unlikely(row == nullptr))
    return FPTA_EINVAL;

  row->total_bytes = 0;
  row->units = nullptr;

  if (unlikely(!fpta_cursor_validate(cursor, fpta_read)))
    return FPTA_EINVAL;

  if (unlikely(!cursor->is_filled()))
    return cursor->unladed_state();

  if (fpta_index_is_primary(cursor->index.shove))
    return mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, &row->sys,
                           MDB_GET_CURRENT);

  MDB_val pk_key;
  int rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, &pk_key,
                           MDB_GET_CURRENT);
  if (unlikely(rc != MDB_SUCCESS))
    return rc;

  rc = mdbx_get(cursor->txn->mdbx_txn, cursor->table_id->mdbx_dbi, &pk_key,
                &row->sys);
  return (rc != MDB_NOTFOUND) ? rc : (int)FPTA_INDEX_CORRUPTED;
}

int fpta_cursor_key(fpta_cursor *cursor, fpta_value *key) {
  if (unlikely(key == nullptr))
    return FPTA_EINVAL;
  if (unlikely(!fpta_cursor_validate(cursor, fpta_read)))
    return FPTA_EINVAL;

  if (unlikely(!cursor->is_filled()))
    return cursor->unladed_state();

  int rc = fpta_index_key2value(cursor->index.shove, cursor->current, *key);
  return rc;
}

int fpta_cursor_delete(fpta_cursor *cursor) {
  if (unlikely(!fpta_cursor_validate(cursor, fpta_write)))
    return FPTA_EINVAL;

  if (unlikely(!cursor->is_filled()))
    return cursor->unladed_state();

  if (!fpta_table_has_secondary(cursor->table_id)) {
    int rc = mdbx_cursor_del(cursor->mdbx_cursor, 0);
    if (unlikely(rc != FPTA_SUCCESS)) {
      cursor->set_poor();
      return rc;
    }
  } else {
    MDB_val pk_key;
    if (fpta_index_is_primary(cursor->index.shove)) {
      pk_key = cursor->current;
      if (pk_key.iov_len > 0 &&
          /* LY: FIXME тут можно убрать вызов mdbx_is_dirty() и просто
           * всегда копировать ключ, так как это скорее всего дешевле. */
          mdbx_is_dirty(cursor->txn->mdbx_txn, pk_key.iov_base) !=
              MDBX_RESULT_FALSE) {
        void *buffer = alloca(pk_key.iov_len);
        pk_key.iov_base = memcpy(buffer, pk_key.iov_base, pk_key.iov_len);
      }
    } else {
      int rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, &pk_key,
                               MDB_GET_CURRENT);
      if (unlikely(rc != MDB_SUCCESS)) {
        cursor->set_poor();
        return (rc != MDB_NOTFOUND) ? rc : (int)FPTA_INDEX_CORRUPTED;
      }
    }

    fptu_ro old;
#if defined(NDEBUG) && !defined(_MSC_VER)
    const constexpr size_t likely_enough = 64u * 42u;
#else
    const size_t likely_enough = (time(nullptr) & 1) ? 11u : 64u * 42u;
#endif /* NDEBUG */
    void *buffer = alloca(likely_enough);
    old.sys.iov_base = buffer;
    old.sys.iov_len = likely_enough;

    int rc = mdbx_replace(cursor->txn->mdbx_txn, cursor->table_id->mdbx_dbi,
                          &pk_key, nullptr, &old.sys, MDB_CURRENT);
    if (unlikely(rc == MDBX_RESULT_TRUE)) {
      assert(old.sys.iov_base == nullptr && old.sys.iov_len > likely_enough);
      old.sys.iov_base = alloca(old.sys.iov_len);
      rc = mdbx_replace(cursor->txn->mdbx_txn, cursor->table_id->mdbx_dbi,
                        &pk_key, nullptr, &old.sys, MDB_CURRENT);
    }
    if (unlikely(rc != MDB_SUCCESS)) {
      cursor->set_poor();
      return rc;
    }

    rc = fpta_secondary_remove(cursor->txn, cursor->table_id, pk_key, old,
                               cursor->index.column_order);
    if (unlikely(rc != MDB_SUCCESS)) {
      cursor->set_poor();
      return fpta_inconsistent_abort(cursor->txn, rc);
    }

    if (!fpta_index_is_primary(cursor->index.shove)) {
      rc = mdbx_cursor_del(cursor->mdbx_cursor, 0);
      if (unlikely(rc != MDB_SUCCESS)) {
        cursor->set_poor();
        return fpta_inconsistent_abort(cursor->txn, rc);
      }
    }
  }

  if (fpta_cursor_is_descending(cursor->options)) {
    /* Для курсора с обратным порядком строк требуется перейти к предыдущей
     * строке, в том числе подходящей под условие фильтрации. */
    fpta_cursor_seek(cursor, MDB_PREV, MDB_PREV, nullptr, nullptr);
  } else if (mdbx_cursor_eof(cursor->mdbx_cursor) == MDBX_RESULT_TRUE) {
    cursor->set_eof(fpta_cursor::after_last);
  } else {
    /* Для курсора с прямым порядком строк требуется перейти
     * к следующей строке подходящей под условие фильтрации, но
     * не выполнять переход если текущая строка уже подходит под фильтр. */
    fpta_cursor_seek(cursor, MDB_GET_CURRENT, MDB_NEXT, nullptr, nullptr);
  }

  return FPTA_SUCCESS;
}

//----------------------------------------------------------------------------

int fpta_cursor_validate_update(fpta_cursor *cursor, fptu_ro new_row_value) {
  if (unlikely(!fpta_cursor_validate(cursor, fpta_write)))
    return FPTA_EINVAL;

  if (unlikely(!cursor->is_filled()))
    return cursor->unladed_state();

  fpta_key column_key;
  int rc = fpta_index_row2key(cursor->index.shove, cursor->index.column_order,
                              new_row_value, column_key, false);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  if (!fpta_is_same(cursor->current, column_key.mdbx))
    return FPTA_KEY_MISMATCH;

  if (!fpta_table_has_secondary(cursor->table_id))
    return FPTA_SUCCESS;

  fptu_ro present_row;
  if (fpta_index_is_primary(cursor->index.shove)) {
    rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current,
                         &present_row.sys, MDB_GET_CURRENT);
    if (unlikely(rc != MDB_SUCCESS))
      return rc;

    return fpta_check_constraints(cursor->txn, cursor->table_id, present_row,
                                  new_row_value, 0);
  }

  MDB_val present_pk_key;
  rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, &present_pk_key,
                       MDB_GET_CURRENT);
  if (unlikely(rc != MDB_SUCCESS))
    return rc;

  fpta_key new_pk_key;
  rc = fpta_index_row2key(cursor->table_id->table.pk, 0, new_row_value,
                          new_pk_key, false);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  rc = mdbx_get(cursor->txn->mdbx_txn, cursor->table_id->mdbx_dbi,
                &present_pk_key, &present_row.sys);
  if (unlikely(rc != MDB_SUCCESS))
    return (rc != MDB_NOTFOUND) ? rc : (int)FPTA_INDEX_CORRUPTED;

  return fpta_check_constraints(cursor->txn, cursor->table_id, present_row,
                                new_row_value, cursor->index.column_order);
}

int fpta_cursor_update(fpta_cursor *cursor, fptu_ro new_row_value) {
  if (unlikely(!fpta_cursor_validate(cursor, fpta_write)))
    return FPTA_EINVAL;

  if (unlikely(!cursor->is_filled()))
    return cursor->unladed_state();

  fpta_key column_key;
  int rc = fpta_index_row2key(cursor->index.shove, cursor->index.column_order,
                              new_row_value, column_key, false);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

  if (!fpta_is_same(cursor->current, column_key.mdbx))
    return FPTA_KEY_MISMATCH;

  if (!fpta_table_has_secondary(cursor->table_id)) {
    rc = mdbx_cursor_put(cursor->mdbx_cursor, &column_key.mdbx,
                         &new_row_value.sys, MDB_CURRENT | MDB_NODUPDATA);
    if (likely(rc == MDB_SUCCESS) &&
        /* актуализируем текущий ключ, если он был в грязной странице, то при
         * изменении мог быть перемещен с перезаписью старого значения */
        mdbx_is_dirty(cursor->txn->mdbx_txn, cursor->current.iov_base)) {
      rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, nullptr,
                           MDB_GET_CURRENT);
    }
    if (unlikely(rc != MDB_SUCCESS))
      cursor->set_poor();
    return rc;
  }

  MDB_val old_pk_key;
  if (fpta_index_is_primary(cursor->index.shove)) {
    old_pk_key = cursor->current;
  } else {
    rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, &old_pk_key,
                         MDB_GET_CURRENT);
    if (unlikely(rc != MDB_SUCCESS)) {
      cursor->set_poor();
      return (rc != MDB_NOTFOUND) ? rc : (int)FPTA_INDEX_CORRUPTED;
    }
  }

  /* Здесь не очевидный момент при обновлении с изменением PK:
   *  - для обновления secondary индексов требуется как старое,
   *    так и новое значения строки, а также оба значения PK.
   *  - подготовленный old_pk_key содержит указатель на значение,
   *    которое физически размещается в value в служебной таблице
   *    secondary индекса, по которому открыт курсор.
   *  - если сначала, вызовом fpta_secondary_upsert(), обновить
   *    вспомогательные таблицы для secondary индексов, то указатель
   *    внутри old_pk_key может стать невалидным, т.е. так мы потеряем
   *    предыдущее значение PK.
   *  - если же сначала просто обновить строку в основной таблице,
   *    то будет утрачено её предыдущее значение, которое требуется
   *    для обновления secondary индексов.
   *
   * Поэтому, чтобы не потерять старое значение PK и одновременно избежать
   * лишних копирований, здесь используется mdbx_get_ex(). В свою очередь
   * mdbx_get_ex() использует MDB_SET_KEY для получения как данных, так и
   * данных ключа. */

  fptu_ro old;
  rc = mdbx_get_ex(cursor->txn->mdbx_txn, cursor->table_id->mdbx_dbi,
                   &old_pk_key, &old.sys, nullptr);
  if (unlikely(rc != MDB_SUCCESS)) {
    cursor->set_poor();
    return (rc != MDB_NOTFOUND) ? rc : (int)FPTA_INDEX_CORRUPTED;
  }

  fpta_key new_pk_key;
  rc = fpta_index_row2key(cursor->table_id->table.pk, 0, new_row_value,
                          new_pk_key, false);
  if (unlikely(rc != FPTA_SUCCESS))
    return rc;

#if 0 /* LY: в данный момент нет необходимости */
  if (old_pk_key.iov_len > 0 &&
      mdbx_is_dirty(cursor->txn->mdbx_txn, old_pk_key.iov_base) !=
          MDBX_RESULT_FALSE) {
    void *buffer = alloca(old_pk_key.iov_len);
    old_pk_key.iov_base =
        memcpy(buffer, old_pk_key.iov_base, old_pk_key.iov_len);
  }
#endif

  rc = fpta_secondary_upsert(cursor->txn, cursor->table_id, old_pk_key, old,
                             new_pk_key.mdbx, new_row_value,
                             cursor->index.column_order);
  if (unlikely(rc != MDB_SUCCESS)) {
    cursor->set_poor();
    return fpta_inconsistent_abort(cursor->txn, rc);
  }

  const bool pk_changed = !fpta_is_same(old_pk_key, new_pk_key.mdbx);
  if (pk_changed) {
    rc = mdbx_del(cursor->txn->mdbx_txn, cursor->table_id->mdbx_dbi,
                  &old_pk_key, nullptr);
    if (unlikely(rc != MDB_SUCCESS)) {
      cursor->set_poor();
      return fpta_inconsistent_abort(cursor->txn, rc);
    }

    rc = mdbx_put(cursor->txn->mdbx_txn, cursor->table_id->mdbx_dbi,
                  &new_pk_key.mdbx, &new_row_value.sys,
                  MDB_NODUPDATA | MDB_NOOVERWRITE);
    if (unlikely(rc != MDB_SUCCESS)) {
      cursor->set_poor();
      return fpta_inconsistent_abort(cursor->txn, rc);
    }

    rc = mdbx_cursor_put(cursor->mdbx_cursor, &column_key.mdbx,
                         &new_pk_key.mdbx, MDB_CURRENT | MDB_NODUPDATA);

  } else {
    rc = mdbx_put(cursor->txn->mdbx_txn, cursor->table_id->mdbx_dbi,
                  &new_pk_key.mdbx, &new_row_value.sys,
                  MDB_CURRENT | MDB_NODUPDATA);
  }

  if (likely(rc == MDB_SUCCESS) &&
      /* актуализируем текущий ключ, если он был в грязной странице, то при
       * изменении мог быть перемещен с перезаписью старого значения */
      mdbx_is_dirty(cursor->txn->mdbx_txn, cursor->current.iov_base)) {
    rc = mdbx_cursor_get(cursor->mdbx_cursor, &cursor->current, nullptr,
                         MDB_GET_CURRENT);
  }
  if (unlikely(rc != MDB_SUCCESS)) {
    cursor->set_poor();
    return fpta_inconsistent_abort(cursor->txn, rc);
  }

  return FPTA_SUCCESS;
}
