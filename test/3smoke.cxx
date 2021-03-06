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
#include <gtest/gtest.h>

static const char testdb_name[] = "ut_smoke.fpta";
static const char testdb_name_lck[] = "ut_smoke.fpta-lock";

TEST(SmokeIndex, Primary) {
  /* Smoke-проверка жизнеспособности первичных индексов.
   *
   * Сценарий:
   *  1. Создаем базу с одной таблицей, в которой три колонки
   *     и один (primary) индекс.
   *  2. Добавляем данные:
   *     - добавляем "первую" запись, одновременно пытаясь
   *       добавить в строку-кортеж поля с "плохими" значениями.
   *     - добавляем "вторую" запись, которая отличается от первой
   *       всеми колонками.
   *     - также попутно пытаемся обновить несуществующие записи
   *       и вставить дубликаты.
   *  3. Читаем добавленное:
   *     - открываем курсор по основному индексу, без фильтра,
   *       на всю таблицу (весь диапазон строк),
   *       и проверяем кол-во записей и дубликатов.
   *     - переходим к последней, читаем и проверяем её (должна быть
   *       "вторая").
   *     - переходим к первой, читаем и проверяем её (должна быть "первая").
   *  4. Удаляем данные:
   *     - сначала "вторую" запись, потом "первую".
   *     - проверяем кол-во записей и дубликатов, eof для курсора.
   *  5. Завершаем операции и освобождаем ресурсы.
   */
  ASSERT_TRUE(unlink(testdb_name) == 0 || errno == ENOENT);
  ASSERT_TRUE(unlink(testdb_name_lck) == 0 || errno == ENOENT);

  // открываем/создаем базульку в 1 мегабайт
  fpta_db *db = nullptr;
  EXPECT_EQ(FPTA_SUCCESS,
            fpta_db_open(testdb_name, fpta_async, 0644, 1, true, &db));
  ASSERT_NE(nullptr, db);

  // описываем простейшую таблицу с тремя колонками и одним PK
  fpta_column_set def;
  fpta_column_set_init(&def);

  EXPECT_EQ(FPTA_OK,
            fpta_column_describe("pk_str_uniq", fptu_cstr, fpta_primary, &def));
  EXPECT_EQ(FPTA_OK,
            fpta_column_describe("a_uint", fptu_uint64, fpta_index_none, &def));
  EXPECT_EQ(FPTA_OK,
            fpta_column_describe("b_fp", fptu_fp64, fpta_index_none, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));

  // запускам транзакцию и создаем таблицу с обозначенным набором колонок
  fpta_txn *txn = (fpta_txn *)&txn;
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_schema, &txn));
  ASSERT_NE(nullptr, txn);
  EXPECT_EQ(FPTA_OK, fpta_table_create(txn, "table_1", &def));
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  txn = nullptr;

  // инициализируем идентификаторы таблицы и её колонок
  fpta_name table, col_pk, col_a, col_b;
  EXPECT_EQ(FPTA_OK, fpta_table_init(&table, "table_1"));
  EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_pk, "pk_str_uniq"));
  EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_a, "a_uint"));
  EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_b, "b_fp"));

  // начинаем транзакцию для вставки данных
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_write, &txn));
  ASSERT_NE(nullptr, txn);
  // ради теста делаем привязку вручную
  EXPECT_EQ(FPTA_OK, fpta_name_refresh_couple(txn, &table, &col_pk));
  EXPECT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_a));
  EXPECT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_b));

  // создаем кортеж, который станет первой записью в таблице
  fptu_rw *pt1 = fptu_alloc(3, 42);
  ASSERT_NE(nullptr, pt1);
  ASSERT_STREQ(nullptr, fptu_check(pt1));

  // ради проверки пытаемся сделать нехорошее (добавить поля с нарушениями)
  EXPECT_EQ(FPTA_ETYPE, fpta_upsert_column(pt1, &col_pk, fpta_value_uint(12)));
  EXPECT_EQ(FPTA_EVALUE, fpta_upsert_column(pt1, &col_a, fpta_value_sint(-34)));
  EXPECT_EQ(FPTA_ETYPE,
            fpta_upsert_column(pt1, &col_b, fpta_value_cstr("string")));

  // добавляем нормальные значения
  EXPECT_EQ(FPTA_OK,
            fpta_upsert_column(pt1, &col_pk, fpta_value_cstr("pk-string")));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt1, &col_a, fpta_value_sint(34)));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt1, &col_b, fpta_value_float(56.78)));
  ASSERT_STREQ(nullptr, fptu_check(pt1));

  // создаем еще один кортеж для второй записи
  fptu_rw *pt2 = fptu_alloc(3, 42);
  ASSERT_NE(nullptr, pt2);
  ASSERT_STREQ(nullptr, fptu_check(pt2));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt2, &col_pk, fpta_value_cstr("zzz")));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt2, &col_a, fpta_value_sint(90)));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt2, &col_b, fpta_value_float(12.34)));
  ASSERT_STREQ(nullptr, fptu_check(pt2));

  // пытаемся обновить несуществующую запись
  EXPECT_EQ(MDB_NOTFOUND,
            fpta_update_row(txn, &table, fptu_take_noshrink(pt1)));
  // вставляем и обновляем
  EXPECT_EQ(FPTA_OK, fpta_insert_row(txn, &table, fptu_take_noshrink(pt1)));
  EXPECT_EQ(FPTA_OK, fpta_upsert_row(txn, &table, fptu_take_noshrink(pt1)));
  EXPECT_EQ(FPTA_OK, fpta_update_row(txn, &table, fptu_take_noshrink(pt1)));
  EXPECT_EQ(MDB_KEYEXIST,
            fpta_insert_row(txn, &table, fptu_take_noshrink(pt1)));

  // аналогично со второй записью
  EXPECT_EQ(MDB_NOTFOUND,
            fpta_update_row(txn, &table, fptu_take_noshrink(pt2)));
  EXPECT_EQ(FPTA_OK, fpta_insert_row(txn, &table, fptu_take_noshrink(pt2)));
  EXPECT_EQ(FPTA_OK, fpta_upsert_row(txn, &table, fptu_take_noshrink(pt2)));
  EXPECT_EQ(FPTA_OK, fpta_update_row(txn, &table, fptu_take_noshrink(pt2)));
  EXPECT_EQ(MDB_KEYEXIST,
            fpta_insert_row(txn, &table, fptu_take_noshrink(pt2)));

  // фиксируем изменения
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  // и начинаем следующую транзакцию
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_write, &txn));
  ASSERT_NE(nullptr, txn);

  // открываем простейщий курсор: на всю таблицу, без фильтра
  fpta_cursor *cursor;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn, &col_pk, fpta_value_begin(), fpta_value_end(),
                             nullptr, fpta_unsorted_dont_fetch, &cursor));
  ASSERT_NE(nullptr, cursor);

  // узнам сколько записей за курсором (в таблице).
  size_t count;
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(2, count);

  // переходим к последней записи
  EXPECT_EQ(FPTA_OK, fpta_cursor_move(cursor, fpta_last));
  // ради проверки убеждаемся что за курсором есть данные
  EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));

  // считаем повторы, их не должно быть
  size_t dups;
  EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
  EXPECT_EQ(1, dups);

  // получаем текущую строку, она должна совпадать со вторым кортежем
  fptu_ro row2;
  EXPECT_EQ(FPTA_OK, fpta_cursor_get(cursor, &row2));
  ASSERT_STREQ(nullptr, fptu_check_ro(row2));
  EXPECT_EQ(fptu_eq, fptu_cmp_tuples(fptu_take_noshrink(pt2), row2));

  // позиционируем курсор на конкретное значение ключевого поля
  fpta_value pk = fpta_value_cstr("pk-string");
  EXPECT_EQ(FPTA_OK, fpta_cursor_locate(cursor, true, &pk, nullptr));
  EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));

  // ради проверки считаем повторы
  EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
  EXPECT_EQ(1, dups);

  // получаем текущую строку, она должна совпадать с первым кортежем
  fptu_ro row1;
  EXPECT_EQ(FPTA_OK, fpta_cursor_get(cursor, &row1));
  ASSERT_STREQ(nullptr, fptu_check_ro(row1));
  EXPECT_EQ(fptu_eq, fptu_cmp_tuples(fptu_take_noshrink(pt1), row1));

  // разрушаем созданные кортежи
  // на всякий случай предварительно проверяя их
  ASSERT_STREQ(nullptr, fptu_check(pt1));
  free(pt1);
  pt1 = nullptr;
  ASSERT_STREQ(nullptr, fptu_check(pt2));
  free(pt2);
  pt2 = nullptr;

  // удяляем текущую запись через курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_delete(cursor));
  // считаем сколько записей теперь, должа быть одна
  EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
  EXPECT_EQ(1, dups);
  // ради теста проверям что данные есть
  EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(1, count);

  // переходим к первой записи
  EXPECT_EQ(FPTA_OK, fpta_cursor_move(cursor, fpta_first));
  // еще раз удаляем запись
  EXPECT_EQ(FPTA_OK, fpta_cursor_delete(cursor));
#if FPTA_ENABLE_RETURN_INTO_RANGE
  // теперь должно быть пусто
  EXPECT_EQ(FPTA_NODATA, fpta_cursor_dups(cursor, &dups));
  EXPECT_EQ(0, dups);
#else
  // курсор должен стать неустановленным
  EXPECT_EQ(FPTA_ECURSOR, fpta_cursor_dups(cursor, &dups));
  EXPECT_EQ(FPTA_DEADBEEF, dups);
#endif
  // ради теста проверям что данных больше нет
  EXPECT_EQ(FPTA_NODATA, fpta_cursor_eof(cursor));
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(0, count);

  // закрываем курсор и завершаем транзакцию
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor));
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  txn = nullptr;

  // разрушаем привязанные идентификаторы
  fpta_name_destroy(&table);
  fpta_name_destroy(&col_pk);
  fpta_name_destroy(&col_a);
  fpta_name_destroy(&col_b);

  // закрываем базульку
  EXPECT_EQ(FPTA_SUCCESS, fpta_db_close(db));

  // пока не удялем файлы чтобы можно было посмотреть и натравить mdbx_chk
  if (false) {
    ASSERT_TRUE(unlink(testdb_name) == 0);
    ASSERT_TRUE(unlink(testdb_name_lck) == 0);
  }
}

TEST(SmokeIndex, Secondary) {
  /* Smoke-проверка жизнеспособности вторичных индексов.
   *
   * Сценарий:
   *  1. Создаем базу с одной таблицей, в которой три колонки,
   *     и два индекса (primary и secondary).
   *  2. Добавляем данные:
   *      - добавляем "первую" запись, одновременно пытаясь
   *        добавить в строку-кортеж поля с "плохими" значениями.
   *      - добавляем "вторую" запись, которая отличается от первой
   *        всеми колонками.
   *      - также попутно пытаемся обновить несуществующие записи
   *        и вставить дубликаты.
   *  3. Читаем добавленное:
   *     - открываем курсор по вторичному индексу, без фильтра,
   *       на всю таблицу (весь диапазон строк),
   *       и проверяем кол-во записей и дубликатов.
   *     - переходим к последней, читаем и проверяем её (должна быть
   *       "вторая").
   *     - переходим к первой, читаем и проверяем её (должна быть "первая").
   *  4. Удаляем данные:
   *     - сначала "вторую" запись, потом "первую".
   *     - проверяем кол-во записей и дубликатов, eof для курсора.
   *  5. Завершаем операции и освобождаем ресурсы.
   */
  ASSERT_TRUE(unlink(testdb_name) == 0 || errno == ENOENT);
  ASSERT_TRUE(unlink(testdb_name_lck) == 0 || errno == ENOENT);

  // открываем/создаем базульку в 1 мегабайт
  fpta_db *db = nullptr;
  EXPECT_EQ(FPTA_SUCCESS,
            fpta_db_open(testdb_name, fpta_async, 0644, 1, true, &db));
  ASSERT_NE(nullptr, db);

  // описываем простейшую таблицу с тремя колонками,
  // одним Primary и одним Secondary
  fpta_column_set def;
  fpta_column_set_init(&def);

  EXPECT_EQ(FPTA_OK,
            fpta_column_describe("pk_str_uniq", fptu_cstr, fpta_primary, &def));
  EXPECT_EQ(FPTA_OK,
            fpta_column_describe("a_uint", fptu_uint64, fpta_secondary, &def));
  EXPECT_EQ(FPTA_OK,
            fpta_column_describe("b_fp", fptu_fp64, fpta_index_none, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));

  // запускам транзакцию и создаем таблицу с обозначенным набором колонок
  fpta_txn *txn = (fpta_txn *)&txn;
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_schema, &txn));
  ASSERT_NE(nullptr, txn);
  EXPECT_EQ(FPTA_OK, fpta_table_create(txn, "table_1", &def));
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  txn = nullptr;

  // инициализируем идентификаторы таблицы и её колонок
  fpta_name table, col_pk, col_a, col_b;
  EXPECT_EQ(FPTA_OK, fpta_table_init(&table, "table_1"));
  EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_pk, "pk_str_uniq"));
  EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_a, "a_uint"));
  EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_b, "b_fp"));

  // начинаем транзакцию для вставки данных
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_write, &txn));
  ASSERT_NE(nullptr, txn);
  // ради теста делаем привязку вручную
  EXPECT_EQ(FPTA_OK, fpta_name_refresh_couple(txn, &table, &col_pk));
  EXPECT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_a));
  EXPECT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_b));

  // создаем кортеж, который станет первой записью в таблице
  fptu_rw *pt1 = fptu_alloc(3, 42);
  ASSERT_NE(nullptr, pt1);
  ASSERT_STREQ(nullptr, fptu_check(pt1));

  // ради проверки пытаемся сделать нехорошее (добавить поля с нарушениями)
  EXPECT_EQ(FPTA_ETYPE, fpta_upsert_column(pt1, &col_pk, fpta_value_uint(12)));
  EXPECT_EQ(FPTA_EVALUE, fpta_upsert_column(pt1, &col_a, fpta_value_sint(-34)));
  EXPECT_EQ(FPTA_ETYPE,
            fpta_upsert_column(pt1, &col_b, fpta_value_cstr("string")));

  // добавляем нормальные значения
  EXPECT_EQ(FPTA_OK,
            fpta_upsert_column(pt1, &col_pk, fpta_value_cstr("pk-string")));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt1, &col_a, fpta_value_sint(34)));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt1, &col_b, fpta_value_float(56.78)));
  ASSERT_STREQ(nullptr, fptu_check(pt1));

  // создаем еще один кортеж для второй записи
  fptu_rw *pt2 = fptu_alloc(3, 42);
  ASSERT_NE(nullptr, pt2);
  ASSERT_STREQ(nullptr, fptu_check(pt2));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt2, &col_pk, fpta_value_cstr("zzz")));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt2, &col_a, fpta_value_sint(90)));
  EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt2, &col_b, fpta_value_float(12.34)));
  ASSERT_STREQ(nullptr, fptu_check(pt2));

  // пытаемся обновить несуществующую запись
  EXPECT_EQ(MDB_NOTFOUND,
            fpta_update_row(txn, &table, fptu_take_noshrink(pt1)));
  // вставляем и обновляем
  EXPECT_EQ(FPTA_OK, fpta_insert_row(txn, &table, fptu_take_noshrink(pt1)));
  EXPECT_EQ(FPTA_OK, fpta_upsert_row(txn, &table, fptu_take_noshrink(pt1)));
  EXPECT_EQ(FPTA_OK, fpta_update_row(txn, &table, fptu_take_noshrink(pt1)));
  EXPECT_EQ(MDB_KEYEXIST,
            fpta_insert_row(txn, &table, fptu_take_noshrink(pt1)));

  // аналогично со второй записью
  EXPECT_EQ(MDB_NOTFOUND,
            fpta_update_row(txn, &table, fptu_take_noshrink(pt2)));
  EXPECT_EQ(FPTA_OK, fpta_insert_row(txn, &table, fptu_take_noshrink(pt2)));
  EXPECT_EQ(FPTA_OK, fpta_upsert_row(txn, &table, fptu_take_noshrink(pt2)));
  EXPECT_EQ(FPTA_OK, fpta_update_row(txn, &table, fptu_take_noshrink(pt2)));
  EXPECT_EQ(MDB_KEYEXIST,
            fpta_insert_row(txn, &table, fptu_take_noshrink(pt2)));

  // фиксируем изменения
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  // и начинаем следующую транзакцию
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_write, &txn));
  ASSERT_NE(nullptr, txn);

  // открываем простейщий курсор: на всю таблицу, без фильтра
  fpta_cursor *cursor;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn, &col_a, fpta_value_begin(), fpta_value_end(),
                             nullptr, fpta_unsorted_dont_fetch, &cursor));
  ASSERT_NE(nullptr, cursor);

  // узнам сколько записей за курсором (в таблице).
  size_t count;
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(2, count);

  // переходим к первой записи
  EXPECT_EQ(FPTA_OK, fpta_cursor_move(cursor, fpta_first));
  // ради проверки убеждаемся что за курсором есть данные
  EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));

  // считаем повторы, их не должно быть
  size_t dups;
  EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
  ASSERT_EQ(1, dups);

  // переходим к последней записи
  EXPECT_EQ(FPTA_OK, fpta_cursor_move(cursor, fpta_last));
  // ради проверки убеждаемся что за курсором есть данные
  EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));

  // получаем текущую строку, она должна совпадать со вторым кортежем
  fptu_ro row2;
  EXPECT_EQ(FPTA_OK, fpta_cursor_get(cursor, &row2));
  ASSERT_STREQ(nullptr, fptu_check_ro(row2));
  EXPECT_EQ(fptu_eq, fptu_cmp_tuples(fptu_take_noshrink(pt2), row2));

  // считаем повторы, их не должно быть
  EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
  ASSERT_EQ(1, dups);

  // позиционируем курсор на конкретное значение ключевого поля
  fpta_value pk = fpta_value_uint(34);
  EXPECT_EQ(FPTA_OK, fpta_cursor_locate(cursor, true, &pk, nullptr));
  EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));

  // ради проверки считаем повторы
  EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
  EXPECT_EQ(1, dups);

  // получаем текущую строку, она должна совпадать с первым кортежем
  fptu_ro row1;
  EXPECT_EQ(FPTA_OK, fpta_cursor_get(cursor, &row1));
  ASSERT_STREQ(nullptr, fptu_check_ro(row1));
  EXPECT_EQ(fptu_eq, fptu_cmp_tuples(fptu_take_noshrink(pt1), row1));

  // разрушаем созданные кортежи
  // на всякий случай предварительно проверяя их
  ASSERT_STREQ(nullptr, fptu_check(pt1));
  free(pt1);
  pt1 = nullptr;
  ASSERT_STREQ(nullptr, fptu_check(pt2));
  free(pt2);
  pt2 = nullptr;

  // удяляем текущую запись через курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_delete(cursor));
  // считаем сколько записей теперь, должа быть одна
  EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
  EXPECT_EQ(1, dups);
  // ради теста проверям что данные есть
  EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(1, count);

  // переходим к первой записи
  EXPECT_EQ(FPTA_OK, fpta_cursor_move(cursor, fpta_first));
  // еще раз удаляем запись
  EXPECT_EQ(FPTA_OK, fpta_cursor_delete(cursor));
#if FPTA_ENABLE_RETURN_INTO_RANGE
  // теперь должно быть пусто
  EXPECT_EQ(FPTA_NODATA, fpta_cursor_dups(cursor, &dups));
  EXPECT_EQ(0, dups);
#else
  // курсор должен стать неустановленным
  EXPECT_EQ(FPTA_ECURSOR, fpta_cursor_dups(cursor, &dups));
  EXPECT_EQ(FPTA_DEADBEEF, dups);
#endif
  // ради теста проверям что данных больше нет
  EXPECT_EQ(FPTA_NODATA, fpta_cursor_eof(cursor));
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(0, count);

  // закрываем курсор и завершаем транзакцию
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor));
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  txn = nullptr;

  // разрушаем привязанные идентификаторы
  fpta_name_destroy(&table);
  fpta_name_destroy(&col_pk);
  fpta_name_destroy(&col_a);
  fpta_name_destroy(&col_b);

  // закрываем базульку
  EXPECT_EQ(FPTA_SUCCESS, fpta_db_close(db));

  // пока не удялем файлы чтобы можно было посмотреть и натравить mdbx_chk
  if (false) {
    ASSERT_TRUE(unlink(testdb_name) == 0);
    ASSERT_TRUE(unlink(testdb_name_lck) == 0);
  }
}

//----------------------------------------------------------------------------

#include "keygen.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

static unsigned mapdup_order2key(unsigned order, unsigned NNN) {
  unsigned quart = NNN / 4;
  unsigned offset = 0;
  unsigned shift = 0;

  while (order >= quart) {
    offset += quart >> shift++;
    order -= quart;
  }
  return (order >> shift) + offset;
}

unsigned mapdup_order2count(unsigned order, unsigned NNN) {
  unsigned value = mapdup_order2key(order, NNN);

  unsigned count = 1;
  for (unsigned n = order; n < NNN; ++n)
    if (n != order && value == mapdup_order2key(n, NNN))
      count++;

  return count;
}

TEST(Smoke, mapdup_order2key) {
  std::map<unsigned, unsigned> checker;

  const unsigned NNN = 32;
  for (unsigned order = 0; order < 32; ++order) {
    unsigned dup = mapdup_order2key(order, NNN);
    checker[dup] += 1;
  }
  EXPECT_EQ(1, checker[0]);
  EXPECT_EQ(1, checker[1]);
  EXPECT_EQ(1, checker[2]);
  EXPECT_EQ(1, checker[3]);
  EXPECT_EQ(1, checker[4]);
  EXPECT_EQ(1, checker[5]);
  EXPECT_EQ(1, checker[6]);
  EXPECT_EQ(1, checker[7]);
  EXPECT_EQ(2, checker[8]);
  EXPECT_EQ(2, checker[9]);
  EXPECT_EQ(2, checker[10]);
  EXPECT_EQ(2, checker[11]);
  EXPECT_EQ(4, checker[12]);
  EXPECT_EQ(4, checker[13]);
  EXPECT_EQ(8, checker[14]);
  EXPECT_EQ(15, checker.size());
}

/* используем для контроля отдельную структуру, чтобы при проблемах/ошибках
 * явно видеть значения в отладчике. */
struct crud_item {
  unsigned pk_uint;
  double se_real;
  fptu_time time;
  std::string se_str;

  crud_item(unsigned pk, const char *str, double real, fptu_time datetime) {
    pk_uint = pk;
    se_real = real;
    time = datetime;
    se_str.assign(str, strlen(str));
  }

  struct less_pk {
    bool operator()(const crud_item *left, const crud_item *right) const {
      return left->pk_uint < right->pk_uint;
    }
  };
  struct less_str {
    bool operator()(const crud_item *left, const crud_item *right) const {
      return left->se_str < right->se_str;
    }
  };
  struct less_real {
    bool operator()(const crud_item *left, const crud_item *right) const {
      return left->se_real < right->se_real;
    }
  };
};

class SmokeCRUD : public ::testing::Test {
public:
  scoped_db_guard db_quard;
  scoped_txn_guard txn_guard;
  scoped_cursor_guard cursor_guard;
  fpta_name table, col_uint, col_time, col_str, col_real;

  // для проверки набора строк и их порядка
  std::vector<std::unique_ptr<crud_item>> container;
  std::set<crud_item *, crud_item::less_pk> checker_pk_uint;
  std::set<crud_item *, crud_item::less_str> checker_str;
  std::set<crud_item *, crud_item::less_real> checker_real;
  unsigned ndeleted;

  void Check(fpta_cursor *cursor) {
    int move_result = fpta_cursor_move(cursor, fpta_first);
    if (container.size() - ndeleted == 0)
      EXPECT_EQ(FPTA_NODATA, move_result);
    else {
      EXPECT_EQ(FPTA_OK, move_result);
      unsigned count = 0;
      do {
        ASSERT_EQ(FPTA_OK, fpta_cursor_eof(cursor));
        fptu_ro row;
        ASSERT_EQ(FPTA_OK, fpta_cursor_get(cursor, &row));
        SCOPED_TRACE("row #" + std::to_string(count) + ", " +
                     std::to_string(row));
        unsigned row_present = 0;
        for (const auto &item : container) {
          if (!item)
            /* пропускаем удаленные строки */
            continue;
          fpta_value value;
          ASSERT_EQ(FPTA_OK, fpta_get_column(row, &col_uint, &value));
          if (item->pk_uint == value.uint) {
            row_present++;
            ASSERT_EQ(FPTA_OK, fpta_get_column(row, &col_str, &value));
            EXPECT_STREQ(item->se_str.c_str(), value.str);
            ASSERT_EQ(FPTA_OK, fpta_get_column(row, &col_real, &value));
            EXPECT_EQ(item->se_real, value.fp);
            ASSERT_EQ(FPTA_OK, fpta_get_column(row, &col_time, &value));
            EXPECT_EQ(item->time.fixedpoint, value.datetime.fixedpoint);
          }
        }
        ASSERT_EQ(1, row_present);
        count++;
        move_result = fpta_cursor_move(cursor, fpta_next);
        ASSERT_TRUE(move_result == FPTA_OK || move_result == FPTA_NODATA);
      } while (move_result == FPTA_OK);
      EXPECT_EQ(container.size() - ndeleted, count);
    }
  }

  void Check() {
    ASSERT_TRUE(txn_guard.operator bool());

    /* проверяем по PK */ {
      SCOPED_TRACE("check: pk/uint");
      // открываем курсор по col_uint: на всю таблицу, без фильтра
      scoped_cursor_guard guard;
      fpta_cursor *cursor;
      EXPECT_EQ(FPTA_OK,
                fpta_cursor_open(txn_guard.get(), &col_uint, fpta_value_begin(),
                                 fpta_value_end(), nullptr,
                                 fpta_unsorted_dont_fetch, &cursor));
      ASSERT_NE(cursor, nullptr);
      guard.reset(cursor);
      ASSERT_NO_FATAL_FAILURE(Check(cursor));
    }

    /* проверяем по вторичному индексу колонки 'str' */ {
      SCOPED_TRACE("check: se/str");
      // открываем курсор по col_str: на всю таблицу, без фильтра
      scoped_cursor_guard guard;
      fpta_cursor *cursor;
      EXPECT_EQ(FPTA_OK,
                fpta_cursor_open(txn_guard.get(), &col_str, fpta_value_begin(),
                                 fpta_value_end(), nullptr,
                                 fpta_unsorted_dont_fetch, &cursor));
      ASSERT_NE(cursor, nullptr);
      guard.reset(cursor);
      ASSERT_NO_FATAL_FAILURE(Check(cursor));
    }

    /* проверяем по вторичному индексу колонки 'real' */ {
      SCOPED_TRACE("check: se/real");
      // открываем курсор по col_real: на всю таблицу, без фильтра
      scoped_cursor_guard guard;
      fpta_cursor *cursor;
      EXPECT_EQ(FPTA_OK,
                fpta_cursor_open(txn_guard.get(), &col_real, fpta_value_begin(),
                                 fpta_value_end(), nullptr,
                                 fpta_unsorted_dont_fetch, &cursor));
      ASSERT_NE(cursor, nullptr);
      guard.reset(cursor);
      ASSERT_NO_FATAL_FAILURE(Check(cursor));
    }
  }

  virtual void SetUp() {
    SCOPED_TRACE("setup");
    // инициализируем идентификаторы таблицы и её колонок
    EXPECT_EQ(FPTA_OK, fpta_table_init(&table, "table_crud"));
    EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_uint, "uint"));
    EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_time, "time"));
    EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_str, "str"));
    EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_real, "real"));

    // чистим
    ASSERT_TRUE(unlink(testdb_name) == 0 || errno == ENOENT);
    ASSERT_TRUE(unlink(testdb_name_lck) == 0 || errno == ENOENT);
    ndeleted = 0;

    // открываем/создаем базульку в 1 мегабайт
    fpta_db *db = nullptr;
    EXPECT_EQ(FPTA_SUCCESS,
              fpta_db_open(testdb_name, fpta_async, 0644, 1, true, &db));
    ASSERT_NE(nullptr, db);
    db_quard.reset(db);

    // описываем структуру таблицы
    fpta_column_set def;
    fpta_column_set_init(&def);

    EXPECT_EQ(FPTA_OK, fpta_column_describe("time", fptu_datetime,
                                            fpta_index_none, &def));
    EXPECT_EQ(FPTA_OK, fpta_column_describe("uint", fptu_uint32,
                                            fpta_primary_unique, &def));
    EXPECT_EQ(FPTA_OK,
              fpta_column_describe("str", fptu_cstr,
                                   fpta_secondary_unique_reversed, &def));
    EXPECT_EQ(FPTA_OK,
              fpta_column_describe("real", fptu_fp64,
                                   fpta_secondary_withdups_unordered, &def));
    EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));

    // запускам транзакцию и создаем таблицу
    fpta_txn *txn = nullptr;
    EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_schema, &txn));
    ASSERT_NE(nullptr, txn);
    txn_guard.reset(txn);
    ASSERT_EQ(FPTA_OK, fpta_table_create(txn, "table_crud", &def));
    ASSERT_EQ(FPTA_OK, fpta_transaction_end(txn_guard.release(), false));
    txn = nullptr;
  }

  virtual void TearDown() {
    SCOPED_TRACE("teardown");
    // разрушаем привязанные идентификаторы
    fpta_name_destroy(&table);
    fpta_name_destroy(&col_uint);
    fpta_name_destroy(&col_time);
    fpta_name_destroy(&col_str);
    fpta_name_destroy(&col_real);

    // закрываем курсор и завершаем транзакцию
    if (cursor_guard)
      EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
    if (txn_guard)
      ASSERT_EQ(FPTA_OK, fpta_transaction_end(txn_guard.release(), true));
    if (db_quard) {
      // закрываем и удаляем базу
      ASSERT_EQ(FPTA_SUCCESS, fpta_db_close(db_quard.release()));
      ASSERT_TRUE(unlink(testdb_name) == 0);
      ASSERT_TRUE(unlink(testdb_name_lck) == 0);
    }
  }

  static unsigned mesh_order4uint(unsigned n, unsigned NNN) {
    return (37 * n) % NNN;
  }

  static unsigned mesh_order4str(unsigned n, unsigned NNN) {
    return (67 * n + 17) % NNN;
  }

  static unsigned mesh_order4real(unsigned n, unsigned NNN) {
    return (97 * n + 43) % NNN;
  }

  static unsigned mesh_order4update(unsigned n, unsigned NNN) {
    return (11 * n + 23) % NNN;
  }

  static unsigned mesh_order4delete(unsigned n, unsigned NNN) {
    return (5 * n + 13) % NNN;
  }
};

TEST_F(SmokeCRUD, none) {
  /* Smoke-проверка CRUD операций с участием индексов.
   *
   * Сценарий:
   *     Заполняем таблицу и затем обновляем и удаляем часть строк,
   *     как без курсора, так и открывая курсор для каждого из
   *     проиндексированных полей.
   *
   *  1. Создаем базу с одной таблицей, в которой:
   *      - четыре колонки и три индекса.
   *      - первичный индекс, для возможности secondary он должен быть
   *        с контролем уникальности.
   *      - два secondary, из которых один с контролем уникальности,
   *        второй неупорядоченный и "с дубликатами".
   *  2. Добавляем данные:
   *     - последующие шаги требуют не менее 32 строк;
   *     - для колонки с дубликатами реализуем карту: 8x1 (8 уникальных),
   *       4x2 (4 парных дубля), 2x4 (два значения по 4 раза),
   *       1x8 (одно значение 8 раз), это делает mapdup_order2key();
   *  3. Обновляем строки:
   *     - без курсора и без изменения PK: перебираем все комбинации
   *       сохранения/изменения каждой колонки = 7 комбинаций из 3 колонок;
   *     - через курсор по каждому индексу: перебираем все комбинации
   *       сохранения/изменения каждой колонки = 7 комбинаций из 3 колонок
   *       для каждого из трех индексов;
   *     - попутно пробуем сделать обновление с нарушением уникальности.
   *     = итого: обновляем 28 строк.
   *  4. Удаляем строки:
   *     - одну без использования курсора;
   *     - по одной через курсор по каждому индексу;
   *     - делаем это как для обновленных строк, так и для нетронутых.
   *     - попутно пробуем удалить несуществующие строки.
   *     - попутно пробуем удалить через fpta_delete() строки
   *       с существующим PK, но различиями в других колонках.
   *     = итого: удаляем 8 строк, из которых 4 не были обновлены.
   *  5. Проверяем содержимое таблицы и состояние индексов:
   *     - читаем без курсора, fpta_get() для каждого индекса с контролем
   *       уникальности = 3 строки;
   *     - через курсор по каждому индексу ходим по трём строкам (первая,
   *       последняя, туда-сюда), при этом читаем и сверяем значения.
   *  6. Завершаем операции и освобождаем ресурсы.
   */

  // начинаем транзакцию для вставки данных
  fpta_txn *txn = nullptr;
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db_quard.get(), fpta_write, &txn));
  ASSERT_NE(nullptr, txn);
  txn_guard.reset(txn);

  // связываем идентификаторы с ранее созданной схемой
  ASSERT_EQ(FPTA_OK, fpta_name_refresh(txn, &table));
  ASSERT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_uint));
  ASSERT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_time));
  ASSERT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_str));
  ASSERT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_real));

  // инициализируем генератор значений для строковой колонки
  any_keygen keygen(fptu_cstr, fpta_name_colindex(&col_str));

  // создаем кортеж, который будем использовать для заполнения таблицы
  fptu_rw *row = fptu_alloc(4, fpta_max_keylen * 2);
  ASSERT_NE(nullptr, row);
  ASSERT_STREQ(nullptr, fptu_check(row));

  constexpr unsigned NNN = 42;
  /* создаем достаточно кол-во строк для последующих проверок */ {
    SCOPED_TRACE("fill");
    for (unsigned i = 0; i < NNN; ++i) {
      /* перемешиваем, так чтобы у полей был независимый порядок */
      unsigned pk_uint_value = mesh_order4uint(i, NNN);
      unsigned order_se_str = mesh_order4str(i, NNN);
      unsigned order_se_real = mesh_order4real(i, NNN);
      double se_real_value = mapdup_order2key(order_se_real, NNN) / (double)NNN;

      SCOPED_TRACE(
          "add: row " + std::to_string(i) + " of [0.." + std::to_string(NNN) +
          "), orders: " + std::to_string(pk_uint_value) + " / " +
          std::to_string(order_se_str) + " / " + std::to_string(order_se_real) +
          " (" + std::to_string(se_real_value) + ")");
      ASSERT_EQ(FPTU_OK, fptu_clear(row));

      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_uint,
                                            fpta_value_uint(pk_uint_value)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_real,
                                            fpta_value_float(se_real_value)));

      /* пытаемся обновить несуществующую строку */
      EXPECT_EQ(MDB_NOTFOUND, fpta_probe_and_update_row(
                                  txn, &table, fptu_take_noshrink(row)));

      /* пытаемся вставить неполноценную строку, в которой сейчас
       * не хватает одного из индексируемых полей, поэтому вместо
       * MDB_NOTFOUND должно быть возвращено FPTA_COLUMN_MISSING */
      EXPECT_EQ(FPTA_COLUMN_MISSING, fpta_probe_and_upsert_row(
                                         txn, &table, fptu_take_noshrink(row)));
      EXPECT_EQ(FPTA_COLUMN_MISSING, fpta_probe_and_insert_row(
                                         txn, &table, fptu_take_noshrink(row)));

      /* добавляем недостающее индексируемое поле */
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_str,
                                            keygen.make(order_se_str, NNN)));

      /* теперь вставляем новую запись, но пока без поля `time`.
       * проверяем как insert, так и upsert. */
      if (i & 1)
        EXPECT_EQ(FPTA_OK,
                  fpta_insert_row(txn, &table, fptu_take_noshrink(row)));
      else
        EXPECT_EQ(FPTA_OK,
                  fpta_upsert_row(txn, &table, fptu_take_noshrink(row)));

      /* пробуем вставить дубликат */
      EXPECT_EQ(MDB_KEYEXIST, fpta_probe_and_insert_row(
                                  txn, &table, fptu_take_noshrink(row)));

      /* добавляем поле `time` с нулевым значением и обновлем */
      fptu_time datetime;
      datetime.fixedpoint = 0;
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_time,
                                            fpta_value_datetime(datetime)));
      EXPECT_EQ(FPTA_OK, fpta_update_row(txn, &table, fptu_take_noshrink(row)));

      /* обновляем поле `time`, проверяя как update, так и upsert. */
      datetime = fptu_now_fine();
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_time,
                                            fpta_value_datetime(datetime)));
      if (i & 2)
        EXPECT_EQ(FPTA_OK, fpta_probe_and_update_row(txn, &table,
                                                     fptu_take_noshrink(row)));
      else
        EXPECT_EQ(FPTA_OK, fpta_probe_and_upsert_row(txn, &table,
                                                     fptu_take_noshrink(row)));

      /* еще раз пробуем вставить дубликат */
      EXPECT_EQ(MDB_KEYEXIST, fpta_probe_and_insert_row(
                                  txn, &table, fptu_take_noshrink(row)));

      /* обновляем PK и пробуем вставить дубликат по вторичным ключам */
      ASSERT_EQ(FPTA_OK,
                fpta_upsert_column(row, &col_uint, fpta_value_uint(NNN)));
      EXPECT_EQ(MDB_KEYEXIST, fpta_probe_and_insert_row(
                                  txn, &table, fptu_take_noshrink(row)));

      // добавляем аналог строки в проверочный набор
      fpta_value se_str_value;
      ASSERT_EQ(FPTA_OK, fpta_get_column(fptu_take_noshrink(row), &col_str,
                                         &se_str_value));
      container.emplace_back(new crud_item(pk_uint_value, se_str_value.str,
                                           se_real_value, datetime));

      checker_pk_uint.insert(container.back().get());
      checker_str.insert(container.back().get());
      checker_real.insert(container.back().get());
    }
  }

  // фиксируем транзакцию и добавленные данные
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn_guard.release(), false));
  txn = nullptr;

  //--------------------------------------------------------------------------

  /* При добавлении строк значения полей были перемешаны (сгенерированы в
   * нелинейном порядке), поэтому из container их можно брать просто
   * последовательно. Однако, для параметризируемой стохастичности теста
   * порядок будет еще раз перемешан посредством mesh_order4update(). */
  unsigned nn = 0;

  // начинаем транзакцию для проверочных обновлений
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db_quard.get(), fpta_write, &txn));
  ASSERT_NE(nullptr, txn);
  txn_guard.reset(txn);

  ASSERT_NO_FATAL_FAILURE(Check());

  /* обновляем строки без курсора и без изменения PK */ {
    SCOPED_TRACE("update.without-cursor");
    for (int m = 0; m < 8; ++m) {
      const unsigned n = mesh_order4update(nn++, NNN);
      SCOPED_TRACE("item " + std::to_string(n) + " of [0.." +
                   std::to_string(NNN) + "), change-mask: " +
                   std::to_string(m));
      crud_item *item = container[n].get();
      SCOPED_TRACE("row-src: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));
      ASSERT_EQ(FPTU_OK, fptu_clear(row));
      if (m & 1)
        item->se_str += "42";
      if (m & 2)
        item->se_real += 42;
      if (m & 4)
        item->time.fixedpoint += 42;
      SCOPED_TRACE("row-dst: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_str,
                                            fpta_value_str(item->se_str)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_real,
                                            fpta_value_float(item->se_real)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_time,
                                            fpta_value_datetime(item->time)));
      /* пробуем обновить без одного поля */
      EXPECT_EQ(FPTA_COLUMN_MISSING, fpta_probe_and_upsert_row(
                                         txn, &table, fptu_take_noshrink(row)));

      /* обновляем строку */
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_uint,
                                            fpta_value_uint(item->pk_uint)));
      EXPECT_EQ(FPTA_OK, fpta_probe_and_upsert_row(txn, &table,
                                                   fptu_take_noshrink(row)));
      ASSERT_NO_FATAL_FAILURE(Check());
    }
    ASSERT_NO_FATAL_FAILURE(Check());
  }

  /* обновляем строки через курсор по col_str. */ {
    SCOPED_TRACE("update.cursor-ordered_unique_reversed_str");
    // открываем курсор по col_str: на всю таблицу, без фильтра
    fpta_cursor *cursor;
    EXPECT_EQ(FPTA_OK, fpta_cursor_open(txn, &col_str, fpta_value_begin(),
                                        fpta_value_end(), nullptr,
                                        fpta_unsorted_dont_fetch, &cursor));
    ASSERT_NE(nullptr, cursor);
    cursor_guard.reset(cursor);

    for (int m = 0; m < 8; ++m) {
      const unsigned n = mesh_order4update(nn++, NNN);
      SCOPED_TRACE("item " + std::to_string(n) + " of [0.." +
                   std::to_string(NNN) + "), change-mask: " +
                   std::to_string(m));
      crud_item *item = container[n].get();
      SCOPED_TRACE("row-src: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      fpta_value key = fpta_value_str(item->se_str);
      ASSERT_EQ(FPTA_OK, fpta_cursor_locate(cursor, true, &key, nullptr));
      EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));
      // ради проверки считаем повторы
      size_t dups;
      EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
      EXPECT_EQ(1, dups);

      ASSERT_EQ(FPTU_OK, fptu_clear(row));
      if (m & 1)
        item->pk_uint += NNN;
      if (m & 2)
        item->se_real += 42;
      if (m & 4)
        item->time.fixedpoint += 42;
      SCOPED_TRACE("row-dst: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_str,
                                            fpta_value_str(item->se_str)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_real,
                                            fpta_value_float(item->se_real)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_time,
                                            fpta_value_datetime(item->time)));
      /* пробуем обновить без одного поля */
      EXPECT_EQ(FPTA_COLUMN_MISSING,
                fpta_cursor_probe_and_update(cursor, fptu_take_noshrink(row)));

      /* обновляем строку */
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_uint,
                                            fpta_value_uint(item->pk_uint)));
      ASSERT_EQ(FPTA_OK,
                fpta_cursor_probe_and_update(cursor, fptu_take_noshrink(row)));

      ASSERT_NO_FATAL_FAILURE(Check());
    }

    // закрываем курсор
    EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
    cursor = nullptr;

    ASSERT_NO_FATAL_FAILURE(Check());
  }

  /* обновляем строки через курсор по col_real. */ {
    SCOPED_TRACE("update.cursor-se-unordered_withdups_real");
    // открываем курсор по col_real: на всю таблицу, без фильтра
    fpta_cursor *cursor;
    EXPECT_EQ(FPTA_OK, fpta_cursor_open(txn, &col_real, fpta_value_begin(),
                                        fpta_value_end(), nullptr,
                                        fpta_unsorted_dont_fetch, &cursor));
    ASSERT_NE(nullptr, cursor);
    cursor_guard.reset(cursor);

    for (int m = 0; m < 8; ++m) {
      const unsigned n = mesh_order4update(nn++, NNN);
      SCOPED_TRACE("item " + std::to_string(n) + " of [0.." +
                   std::to_string(NNN) + "), change-mask: " +
                   std::to_string(m));
      crud_item *item = container[n].get();
      SCOPED_TRACE("row-src: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      // считаем сколько должно быть повторов
      unsigned expected_dups = 0;
      for (auto const &scan : container)
        if (item->se_real == scan->se_real)
          expected_dups++;

      fpta_value key = fpta_value_float(item->se_real);
      if (expected_dups == 1)
        ASSERT_EQ(FPTA_OK, fpta_cursor_locate(cursor, true, &key, nullptr));
      else {
        /* больше одного значения, точное позиционирование возможно
         * только по ключу не возможно, создаем фейковую строку с PK
         * и искомым значением для поиска */
        ASSERT_EQ(FPTU_OK, fptu_clear(row));
        ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_uint,
                                              fpta_value_uint(item->pk_uint)));
        ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_real, key));
        fptu_ro row_value = fptu_take_noshrink(row);
        /* теперь поиск должен быть успешен */
        ASSERT_EQ(FPTA_OK,
                  fpta_cursor_locate(cursor, true, nullptr, &row_value));
      }
      EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));

      // проверяем кол-во повторов
      size_t dups;
      EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
      EXPECT_EQ(expected_dups, dups);

      ASSERT_EQ(FPTU_OK, fptu_clear(row));
      if (m & 1)
        item->pk_uint += NNN;
      if (m & 2)
        item->se_str += "42";
      if (m & 4)
        item->time.fixedpoint += 42;
      SCOPED_TRACE("row-dst: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_uint,
                                            fpta_value_uint(item->pk_uint)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_real,
                                            fpta_value_float(item->se_real)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_time,
                                            fpta_value_datetime(item->time)));
      /* пробуем обновить без одного поля */
      EXPECT_EQ(FPTA_COLUMN_MISSING,
                fpta_cursor_probe_and_update(cursor, fptu_take_noshrink(row)));

      /* обновляем строку */
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_str,
                                            fpta_value_str(item->se_str)));
      ASSERT_EQ(FPTA_OK,
                fpta_cursor_probe_and_update(cursor, fptu_take_noshrink(row)));
      ASSERT_NO_FATAL_FAILURE(Check());
    }

    // закрываем курсор
    EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
    cursor = nullptr;

    ASSERT_NO_FATAL_FAILURE(Check());
  }

  /* обновляем строки через курсор по col_uint (PK). */ {
    SCOPED_TRACE("update.cursor-pk_uint");
    // открываем курсор по col_uint: на всю таблицу, без фильтра
    fpta_cursor *cursor;
    EXPECT_EQ(FPTA_OK, fpta_cursor_open(txn, &col_uint, fpta_value_begin(),
                                        fpta_value_end(), nullptr,
                                        fpta_unsorted_dont_fetch, &cursor));
    ASSERT_NE(nullptr, cursor);
    cursor_guard.reset(cursor);

    for (int m = 0; m < 8; ++m) {
      const unsigned n = mesh_order4update(nn++, NNN);
      SCOPED_TRACE("item " + std::to_string(n) + " of [0.." +
                   std::to_string(NNN) + "), change-mask: " +
                   std::to_string(m));
      crud_item *item = container[n].get();
      SCOPED_TRACE("row-src: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      fpta_value key = fpta_value_uint(item->pk_uint);
      ASSERT_EQ(FPTA_OK, fpta_cursor_locate(cursor, true, &key, nullptr));
      EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));
      // ради проверки считаем повторы
      size_t dups;
      EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
      EXPECT_EQ(1, dups);

      ASSERT_EQ(FPTU_OK, fptu_clear(row));
      if (m & 1)
        item->se_str += "42";
      if (m & 2)
        item->se_real += 42;
      if (m & 4)
        item->time.fixedpoint += 42;
      SCOPED_TRACE("row-dst: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_str,
                                            fpta_value_str(item->se_str)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_real,
                                            fpta_value_float(item->se_real)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_time,
                                            fpta_value_datetime(item->time)));
      /* пробуем обновить без одного поля */
      EXPECT_EQ(FPTA_COLUMN_MISSING,
                fpta_cursor_probe_and_update(cursor, fptu_take_noshrink(row)));

      /* обновляем строку */
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_uint,
                                            fpta_value_uint(item->pk_uint)));
      EXPECT_EQ(FPTA_OK,
                fpta_cursor_probe_and_update(cursor, fptu_take_noshrink(row)));
      ASSERT_NO_FATAL_FAILURE(Check());
    }

    // закрываем курсор
    EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
    cursor = nullptr;

    ASSERT_NO_FATAL_FAILURE(Check());
  }

  // фиксируем транзакцию и измененные данные
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn_guard.release(), false));
  txn = nullptr;

  //--------------------------------------------------------------------------

  /* При добавлении строк значения полей были перемешаны (сгенерированы в
   * нелинейном порядке), поэтому из container их можно брать просто
   * последовательно. Однако, для параметризируемой стохастичности теста
   * порядок будет еще раз перемешан посредством mesh_order4delete(). */
  nn = 0;

  /* за четыре подхода удаляем половину от добавленных строк. */
  const int ndel = NNN / 2 / 4;

  // начинаем транзакцию для проверочных удалений
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db_quard.get(), fpta_write, &txn));
  ASSERT_NE(nullptr, txn);
  txn_guard.reset(txn);

  /* удаляем строки без курсора */ {
    SCOPED_TRACE("delete.without-cursor");

    for (int i = 0; i < ndel; ++i) {
      const unsigned n = mesh_order4delete(nn++, NNN);
      SCOPED_TRACE("item " + std::to_string(n) + " of [0.." +
                   std::to_string(NNN) + "), step #" + std::to_string(i));
      crud_item *item = container[n].get();
      ASSERT_NE(nullptr, item);
      SCOPED_TRACE("row: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));
      ASSERT_EQ(FPTU_OK, fptu_clear(row));

      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_str,
                                            fpta_value_str(item->se_str)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_real,
                                            fpta_value_float(item->se_real)));
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_uint,
                                            fpta_value_uint(item->pk_uint)));

      /* пробуем удалить без одного поля */
      EXPECT_EQ(MDB_NOTFOUND,
                fpta_delete(txn, &table, fptu_take_noshrink(row)));
      /* пробуем удалить с различием в данных (поле time) */
      ASSERT_EQ(FPTA_OK,
                fpta_upsert_column(row, &col_time,
                                   fpta_value_datetime(fptu_now_fine())));
      EXPECT_EQ(MDB_NOTFOUND,
                fpta_delete(txn, &table, fptu_take_noshrink(row)));

      /* пробуем удалить с другим различием в данных (поле real) */
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_time,
                                            fpta_value_datetime(item->time)));
      ASSERT_EQ(FPTA_OK,
                fpta_upsert_column(row, &col_real,
                                   fpta_value_float(item->se_real + 42)));
      EXPECT_EQ(MDB_NOTFOUND,
                fpta_delete(txn, &table, fptu_take_noshrink(row)));

      /* устряняем расхождение и удаляем */
      ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_real,
                                            fpta_value_float(item->se_real)));
      EXPECT_EQ(FPTA_OK, fpta_delete(txn, &table, fptu_take_noshrink(row)));

      container[n].reset();
      ndeleted++;

      ASSERT_NO_FATAL_FAILURE(Check());
    }

    ASSERT_NO_FATAL_FAILURE(Check());
  }

  /* удаляем строки через курсор по col_str. */ {
    SCOPED_TRACE("delete.cursor-ordered_unique_reversed_str");
    // открываем курсор по col_str: на всю таблицу, без фильтра
    fpta_cursor *cursor;
    EXPECT_EQ(FPTA_OK, fpta_cursor_open(txn, &col_str, fpta_value_begin(),
                                        fpta_value_end(), nullptr,
                                        fpta_unsorted_dont_fetch, &cursor));
    ASSERT_NE(nullptr, cursor);
    cursor_guard.reset(cursor);

    for (int i = 0; i < ndel; ++i) {
      const unsigned n = mesh_order4delete(nn++, NNN);
      SCOPED_TRACE("item " + std::to_string(n) + " of [0.." +
                   std::to_string(NNN) + "), step #" + std::to_string(i));
      crud_item *item = container[n].get();
      ASSERT_NE(nullptr, item);
      SCOPED_TRACE("row: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      fpta_value key = fpta_value_str(item->se_str);
      ASSERT_EQ(FPTA_OK, fpta_cursor_locate(cursor, true, &key, nullptr));
      EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));
      // ради проверки считаем повторы
      size_t dups;
      EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
      EXPECT_EQ(1, dups);

      ASSERT_EQ(FPTA_OK, fpta_cursor_delete(cursor));
      ASSERT_EQ(FPTA_NODATA, fpta_cursor_locate(cursor, true, &key, nullptr));
      EXPECT_EQ(FPTA_NODATA, fpta_cursor_eof(cursor));
      EXPECT_EQ(FPTA_ECURSOR, fpta_cursor_dups(cursor, &dups));
      EXPECT_EQ(FPTA_DEADBEEF, dups);

      /* LY: удалять элемент нужно после использования key, так как
       * в key просто указатель на данные std::string, которые будут
       * освобождены при удалении. */
      container[n].reset();
      ndeleted++;
      ASSERT_NO_FATAL_FAILURE(Check());
    }

    // закрываем курсор
    EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
    cursor = nullptr;

    ASSERT_NO_FATAL_FAILURE(Check());
  }

  /* удаляем строки через курсор по col_real. */ {
    SCOPED_TRACE("delete.cursor-se-unordered_withdups_real");
    // открываем курсор по col_real: на всю таблицу, без фильтра
    fpta_cursor *cursor;
    EXPECT_EQ(FPTA_OK, fpta_cursor_open(txn, &col_real, fpta_value_begin(),
                                        fpta_value_end(), nullptr,
                                        fpta_unsorted_dont_fetch, &cursor));
    ASSERT_NE(nullptr, cursor);
    cursor_guard.reset(cursor);

    for (int i = 0; i < ndel; ++i) {
      const unsigned n = mesh_order4delete(nn++, NNN);
      SCOPED_TRACE("item " + std::to_string(n) + " of [0.." +
                   std::to_string(NNN) + "), step #" + std::to_string(i));
      crud_item *item = container[n].get();
      ASSERT_NE(nullptr, item);
      SCOPED_TRACE("row: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      // считаем сколько должно быть повторов
      unsigned expected_dups = 0;
      for (auto const &scan : container)
        if (scan && item->se_real == scan->se_real)
          expected_dups++;

      fptu_ro row_value;
      fpta_value key = fpta_value_float(item->se_real);
      if (expected_dups == 1)
        ASSERT_EQ(FPTA_OK, fpta_cursor_locate(cursor, true, &key, nullptr));
      else {
        /* больше одного значения, точное позиционирование возможно
         * только по ключу не возможно, создаем фейковую строку с PK
         * и искомым значением для поиска */
        ASSERT_EQ(FPTU_OK, fptu_clear(row));
        ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_uint,
                                              fpta_value_uint(item->pk_uint)));
        ASSERT_EQ(FPTA_OK, fpta_upsert_column(row, &col_real, key));
        row_value = fptu_take_noshrink(row);
        /* теперь поиск должен быть успешен */
        ASSERT_EQ(FPTA_OK,
                  fpta_cursor_locate(cursor, true, nullptr, &row_value));
      }
      EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));

      // проверяем кол-во повторов
      size_t dups;
      EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
      EXPECT_EQ(expected_dups, dups);

      ASSERT_EQ(FPTA_OK, fpta_cursor_delete(cursor));
      container[n].reset();
      ndeleted++;

      if (--expected_dups == 0) {
        ASSERT_EQ(FPTA_NODATA, fpta_cursor_locate(cursor, true, &key, nullptr));
        EXPECT_EQ(FPTA_NODATA, fpta_cursor_eof(cursor));
        EXPECT_EQ(FPTA_ECURSOR, fpta_cursor_dups(cursor, &dups));
        EXPECT_EQ(FPTA_DEADBEEF, dups);
      } else {
        ASSERT_EQ(FPTA_OK, fpta_cursor_locate(cursor, true, &key, nullptr));
        EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));
        EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
        EXPECT_EQ(expected_dups, dups);
      }

      ASSERT_NO_FATAL_FAILURE(Check());
    }

    // закрываем курсор
    EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
    cursor = nullptr;

    ASSERT_NO_FATAL_FAILURE(Check());
  }

  /* удаляем строки через курсор по col_uint (PK). */ {
    SCOPED_TRACE("delete.cursor-pk_uint");
    // открываем курсор по col_uint: на всю таблицу, без фильтра
    fpta_cursor *cursor;
    EXPECT_EQ(FPTA_OK, fpta_cursor_open(txn, &col_uint, fpta_value_begin(),
                                        fpta_value_end(), nullptr,
                                        fpta_unsorted_dont_fetch, &cursor));
    ASSERT_NE(nullptr, cursor);
    cursor_guard.reset(cursor);

    for (int i = 0; i < ndel; ++i) {
      const unsigned n = mesh_order4delete(nn++, NNN);
      SCOPED_TRACE("item " + std::to_string(n) + " of [0.." +
                   std::to_string(NNN) + "), step #" + std::to_string(i));
      crud_item *item = container[n].get();
      ASSERT_NE(nullptr, item);
      SCOPED_TRACE("row: pk " + std::to_string(item->pk_uint) + ", str \"" +
                   item->se_str + "\", real " + std::to_string(item->se_real) +
                   ", time " + std::to_string(item->time));

      fpta_value key = fpta_value_uint(item->pk_uint);
      ASSERT_EQ(FPTA_OK, fpta_cursor_locate(cursor, true, &key, nullptr));
      EXPECT_EQ(FPTA_OK, fpta_cursor_eof(cursor));
      // ради проверки считаем повторы
      size_t dups;
      EXPECT_EQ(FPTA_OK, fpta_cursor_dups(cursor, &dups));
      EXPECT_EQ(1, dups);

      ASSERT_EQ(FPTA_OK, fpta_cursor_delete(cursor));
      container[n].reset();
      ndeleted++;

      ASSERT_EQ(FPTA_NODATA, fpta_cursor_locate(cursor, true, &key, nullptr));
      EXPECT_EQ(FPTA_NODATA, fpta_cursor_eof(cursor));
      EXPECT_EQ(FPTA_ECURSOR, fpta_cursor_dups(cursor, &dups));
      EXPECT_EQ(FPTA_DEADBEEF, dups);

      ASSERT_NO_FATAL_FAILURE(Check());
    }

    // закрываем курсор
    EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
    cursor = nullptr;

    ASSERT_NO_FATAL_FAILURE(Check());
  }

  // фиксируем транзакцию и удаление данных
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn_guard.release(), false));
  txn = nullptr;

  //--------------------------------------------------------------------------

  // начинаем транзакцию для финальной проверки
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db_quard.get(), fpta_read, &txn));
  ASSERT_NE(nullptr, txn);
  txn_guard.reset(txn);

  ASSERT_NO_FATAL_FAILURE(Check());

  // закрываем транзакцию
  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn_guard.release(), false));
  txn = nullptr;

  free(row);
  row = nullptr;
}

//----------------------------------------------------------------------------

class SmokeSelect : public ::testing::TestWithParam<
#if GTEST_USE_OWN_TR1_TUPLE || GTEST_HAS_TR1_TUPLE
                        std::tr1::tuple<fpta_index_type, fpta_cursor_options>>
#else
                        std::tuple<fpta_index_type, fpta_cursor_options>>
#endif
{
public:
  scoped_db_guard db_quard;
  scoped_txn_guard txn_guard;
  scoped_cursor_guard cursor_guard;

  fpta_name table, col_1, col_2;
  fpta_index_type index;
  fpta_cursor_options ordering;
  bool valid_ops;

  unsigned count_value_3;
  virtual void SetUp() {
#if GTEST_USE_OWN_TR1_TUPLE || GTEST_HAS_TR1_TUPLE
    index = std::tr1::get<0>(GetParam());
    ordering = std::tr1::get<1>(GetParam());
#else
    index = std::get<0>(GetParam());
    ordering = std::get<1>(GetParam());
#endif
    valid_ops =
        is_valid4primary(fptu_int32, index) && is_valid4cursor(index, ordering);
    ordering = (fpta_cursor_options)(ordering | fpta_dont_fetch);

    SCOPED_TRACE("index " + std::to_string(index) + ", ordering " +
                 std::to_string(ordering) +
                 (valid_ops ? ", (valid case)" : ", (invalid case)"));

    // инициализируем идентификаторы таблицы и её колонок
    EXPECT_EQ(FPTA_OK, fpta_table_init(&table, "table"));
    EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_1, "col_1"));
    EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_2, "col_2"));

    if (!valid_ops)
      return;

    ASSERT_TRUE(unlink(testdb_name) == 0 || errno == ENOENT);
    ASSERT_TRUE(unlink(testdb_name_lck) == 0 || errno == ENOENT);

    // открываем/создаем базульку в 1 мегабайт
    fpta_db *db = nullptr;
    EXPECT_EQ(FPTA_SUCCESS,
              fpta_db_open(testdb_name, fpta_async, 0644, 1, true, &db));
    ASSERT_NE(nullptr, db);
    db_quard.reset(db);

    // описываем простейшую таблицу с двумя колонками
    fpta_column_set def;
    fpta_column_set_init(&def);

    EXPECT_EQ(FPTA_OK, fpta_column_describe("col_1", fptu_int32, index, &def));
    EXPECT_EQ(FPTA_OK,
              fpta_column_describe("col_2", fptu_int32, fpta_index_none, &def));
    EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));

    // запускам транзакцию и создаем таблицу с обозначенным набором колонок
    fpta_txn *txn = (fpta_txn *)&txn;
    EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_schema, &txn));
    ASSERT_NE(nullptr, txn);
    txn_guard.reset(txn);
    EXPECT_EQ(FPTA_OK, fpta_table_create(txn, "table", &def));
    EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn_guard.release(), false));
    txn = nullptr;

    // начинаем транзакцию для вставки данных
    EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_write, &txn));
    ASSERT_NE(nullptr, txn);
    txn_guard.reset(txn);

    // создаем кортеж, который станет первой записью в таблице
    fptu_rw *pt = fptu_alloc(3, 42);
    ASSERT_NE(nullptr, pt);
    ASSERT_STREQ(nullptr, fptu_check(pt));

    // делаем привязку к схеме
    fpta_name_refresh_couple(txn, &table, &col_1);
    fpta_name_refresh(txn, &col_2);

    count_value_3 = 0;
    for (unsigned n = 0; n < 42; ++n) {
      EXPECT_EQ(FPTA_OK, fpta_upsert_column(pt, &col_1, fpta_value_sint(n)));
      unsigned value = (n + 3) % 5;
      count_value_3 += (value == 3);
      EXPECT_EQ(FPTA_OK,
                fpta_upsert_column(pt, &col_2, fpta_value_sint(value)));
      ASSERT_STREQ(nullptr, fptu_check(pt));

      ASSERT_EQ(FPTA_OK, fpta_insert_row(txn, &table, fptu_take_noshrink(pt)));
      // EXPECT_EQ(FPTA_OK, fptu_clear(pt));
    }

    free(pt);
    pt = nullptr;

    // фиксируем изменения
    EXPECT_EQ(FPTA_OK, fpta_transaction_commit(txn_guard.release()));
    txn = nullptr;

    // начинаем следующую транзакцию
    EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_read, &txn));
    ASSERT_NE(nullptr, txn);
    txn_guard.reset(txn);
  }

  virtual void TearDown() {
    SCOPED_TRACE("teardown");

    // разрушаем привязанные идентификаторы
    fpta_name_destroy(&table);
    fpta_name_destroy(&col_1);
    fpta_name_destroy(&col_2);

    // закрываем курсор и завершаем транзакцию
    if (cursor_guard)
      EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
    if (txn_guard)
      ASSERT_EQ(FPTA_OK, fpta_transaction_end(txn_guard.release(), true));
    if (db_quard) {
      // закрываем и удаляем базу
      ASSERT_EQ(FPTA_SUCCESS, fpta_db_close(db_quard.release()));
      ASSERT_TRUE(unlink(testdb_name) == 0);
      ASSERT_TRUE(unlink(testdb_name_lck) == 0);
    }
  }
};

TEST_P(SmokeSelect, Range) {
  /* Smoke-проверка жизнеспособности курсоров с ограничениями диапазона.
   *
   * Сценарий:
   *  1. Создаем базу с одной таблицей, в которой две колонки
   *     и один (primary) индекс.
   *
   *  2. Вставляем 42 строки, с последовательным увеличением
   *     значения в первой колонке.
   *
   *  3. Несколько раз открываем курсор с разнымм диапазонами
   *     и проверяем кол-во строк попадающее в выборку.
   *
   *  4. Завершаем операции и освобождаем ресурсы.
   */

  SCOPED_TRACE("index " + std::to_string(index) + ", ordering " +
               std::to_string(ordering) +
               (valid_ops ? ", (valid case)" : ", (invalid case)"));

  if (!valid_ops)
    return;

  // открываем простейщий курсор БЕЗ диапазона
  fpta_cursor *cursor;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_end(), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  size_t count;
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(42, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем простейщий курсор c диапазоном (полное покрытие)
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(-1),
                             fpta_value_sint(43), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(42, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем c диапазоном (нулевое пересечение, курсор "ниже")
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(-42),
                             fpta_value_sint(0), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(0, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем c диапазоном (нулевое пересечение, курсор "выше")
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(42),
                             fpta_value_sint(100), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(0, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем c диапазоном (единичное пересечение, курсор "снизу")
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(-42),
                             fpta_value_sint(1), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(1, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем c диапазоном (единичное пересечение, курсор "сверху")
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(41),
                             fpta_value_sint(100), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(1, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем c диапазоном (пересечение 50%, курсор "снизу")
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(-100),
                             fpta_value_sint(21), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(21, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем c диапазоном (пересечение 50%, курсор "сверху")
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(21),
                             fpta_value_sint(100), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(21, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем c диапазоном (пересечение 50%, курсор "внутри")
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(10),
                             fpta_value_sint(31), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(21, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем c диапазоном (без пересечения, пустой диапазон)
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(17),
                             fpta_value_sint(17), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(0, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем c диапазоном (без пересечения, "отрицательный" диапазон)
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_sint(31),
                             fpta_value_sint(10), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(0, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;
}

static bool filter_row_predicate_true(const fptu_ro *, void *, void *) {
  return true;
}

static bool filter_row_predicate_false(const fptu_ro *, void *, void *) {
  return false;
}

static bool filter_col_predicate_odd(const fptu_field *column, void *) {
  return (fptu_field_int32(column) & 1) != 0;
}

TEST_P(SmokeSelect, Filter) {
  /* Smoke-проверка жизнеспособности курсоров с фильтром.
   *
   * Сценарий:
   *  1. Создаем базу с одной таблицей, в которой две колонки
   *     и один (primary) индекс.
   *
   *  2. Вставляем 42 строки, с последовательным увеличением
   *     значения в первой колонке.
   *
   *  3. Несколько раз открываем курсор с разными фильтрами
   *     и проверяем кол-во строк попадающее в выборку.
   *
   *  4. Завершаем операции и освобождаем ресурсы.
   */

  SCOPED_TRACE("index " + std::to_string(index) + ", ordering " +
               std::to_string(ordering) +
               (valid_ops ? ", (valid case)" : ", (invalid case)"));

  if (!valid_ops)
    return;

  // открываем простейщий курсор БЕЗ фильтра
  fpta_cursor *cursor;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_end(), nullptr, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  size_t count;
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(42, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем простейщий курсор c псевдо-фильтром (полное покрытие)
  fpta_filter filter;
  filter.type = fpta_node_fnrow;
  filter.node_fnrow.context = nullptr /* unused */;
  filter.node_fnrow.arg = nullptr /* unused */;
  filter.node_fnrow.predicate = filter_row_predicate_true;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_end(), &filter, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(42, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем простейщий курсор c псевдо-фильтром (нулевое покрытие)
  filter.node_fnrow.predicate = filter_row_predicate_false;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_end(), &filter, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(0, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем курсор c фильтром по нечетности значения колонки (покрытие 50%)
  filter.type = fpta_node_fncol;
  filter.node_fncol.column_id = &col_1;
  filter.node_fncol.arg = nullptr /* unused */;
  filter.node_fncol.predicate = filter_col_predicate_odd;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_end(), &filter, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(21, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем курсор c фильтром по значению колонки (равенство)
  filter.type = fpta_node_eq;
  filter.node_cmp.left_id = &col_2;
  filter.node_cmp.right_value = fpta_value_uint(3);
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_end(), &filter, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(count_value_3, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем курсор c фильтром по значению колонки (не равенство)
  filter.type = fpta_node_ne;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_end(), &filter, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(42 - count_value_3, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем курсор c фильтром по значению колонки (больше)
  filter.type = fpta_node_gt;
  filter.node_cmp.left_id = &col_1;
  filter.node_cmp.right_value = fpta_value_uint(10);
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_end(), &filter, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(31, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем курсор c фильтром по значению колонки (меньше)
  filter.type = fpta_node_lt;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_end(), &filter, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(10, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // открываем курсор c тем-же фильтром по значению колонки (меньше)
  // и диапазоном с перекрытием 50% после от фильтра.
  filter.type = fpta_node_lt;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_uint(5), &filter, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(5, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;

  // меняем фильтр на "больше или равно" и открываем курсор с диапазоном,
  // который имеет только одну "общую" запись с условием фильтра.
  filter.type = fpta_node_ge;
  EXPECT_EQ(FPTA_OK,
            fpta_cursor_open(txn_guard.get(), &col_1, fpta_value_begin(),
                             fpta_value_uint(11), &filter, ordering, &cursor));
  ASSERT_NE(nullptr, cursor);
  cursor_guard.reset(cursor);
  // проверяем кол-во записей и закрываем курсор
  EXPECT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  EXPECT_EQ(1, count);
  EXPECT_EQ(FPTA_OK, fpta_cursor_close(cursor_guard.release()));
  cursor = nullptr;
}

#if GTEST_HAS_COMBINE

INSTANTIATE_TEST_CASE_P(
    Combine, SmokeSelect,
    ::testing::Combine(
        ::testing::Values(fpta_primary_unique, fpta_primary_withdups,
                          fpta_primary_unique_unordered,
                          fpta_primary_withdups_unordered),
        ::testing::Values(fpta_unsorted, fpta_ascending, fpta_descending)));
#else

TEST(SmokeSelect, GoogleTestCombine_IS_NOT_Supported_OnThisPlatform) {}

#endif /* GTEST_HAS_COMBINE */

//----------------------------------------------------------------------------

TEST(SmoceCrud, OneRowOneColumn) {
  ASSERT_TRUE(unlink(testdb_name) == 0 || errno == ENOENT);
  ASSERT_TRUE(unlink(testdb_name_lck) == 0 || errno == ENOENT);

  // открываем/создаем базульку в 1 мегабайт
  fpta_db *db = nullptr;
  EXPECT_EQ(FPTA_SUCCESS,
            fpta_db_open(testdb_name, fpta_async, 0644, 1, true, &db));
  ASSERT_NE(nullptr, db);

  // описываем простейшую таблицу с одним PK
  fpta_column_set def;
  fpta_column_set_init(&def);
  ASSERT_EQ(FPTA_OK,
            fpta_column_describe("StrColumn", fptu_cstr, fpta_primary, &def));
  ASSERT_EQ(FPTA_OK, fpta_column_set_validate(&def));

  // запускам транзакцию и создаем таблицу с обозначенным набором колонок
  fpta_txn *txn = nullptr;
  ASSERT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_schema, &txn));
  ASSERT_EQ(FPTA_OK, fpta_table_create(txn, "Table", &def));
  ASSERT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  txn = nullptr;

  // инициализируем идентификаторы таблицы и её колонок
  fpta_name table, col_pk;
  ASSERT_EQ(FPTA_OK, fpta_table_init(&table, "Table"));
  ASSERT_EQ(FPTA_OK, fpta_column_init(&table, &col_pk, "StrColumn"));

  // начинаем транзакцию для вставки данных
  ASSERT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_write, &txn));

  // ради теста делаем привязку вручную
  ASSERT_EQ(FPTA_OK, fpta_name_refresh_couple(txn, &table, &col_pk));

  // создаем кортеж, который станет первой записью в таблице
  fptu_rw *pt1 = fptu_alloc(1, 42);
  ASSERT_NE(nullptr, pt1);
  ASSERT_EQ(nullptr, fptu_check(pt1));

  // добавляем значения колонки
  ASSERT_EQ(FPTA_OK,
            fpta_upsert_column(pt1, &col_pk, fpta_value_cstr("login")));
  ASSERT_EQ(nullptr, fptu_check(pt1));

  // вставляем строку в таблицу
  ASSERT_EQ(FPTA_OK, fpta_upsert_row(txn, &table, fptu_take(pt1)));

  // освобождаем кортеж/строку
  free(pt1);
  pt1 = nullptr;

  // фиксируем изменения
  ASSERT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  txn = nullptr;

  ASSERT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_read, &txn));

  fpta_cursor *cursor = nullptr;
  ASSERT_EQ(FPTA_OK,
            fpta_cursor_open(txn, &col_pk, fpta_value_begin(), fpta_value_end(),
                             nullptr, fpta_unsorted_dont_fetch, &cursor));

  size_t count = 0xBADBADBAD;
  ASSERT_EQ(FPTA_OK, fpta_cursor_count(cursor, &count, INT_MAX));
  ASSERT_EQ(1, count);
  ASSERT_EQ(FPTA_OK, fpta_cursor_close(cursor));

  ASSERT_EQ(FPTA_OK, fpta_transaction_end(txn, false));

  // разрушаем привязанные идентификаторы
  fpta_name_destroy(&table);
  fpta_name_destroy(&col_pk);

  // закрываем базу
  ASSERT_EQ(FPTA_OK, fpta_db_close(db));
}

//----------------------------------------------------------------------------

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
