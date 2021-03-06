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

static const char testdb_name[] = "ut_schema.fpta";
static const char testdb_name_lck[] = "ut_schema.fpta-lock";

TEST(Schema, Trivia) {
  /* Тривиальный тест создания/заполнения описания колонок таблицы.
   *
   * Сценарий:
   *  - создаем/инициализируем описание колонок.
   *  - пробуем добавить несколько некорректных колонок,
   *    с плохими: именем, индексом, типом.
   *  - добавляем несколько корректных описаний колонок.
   *
   * Тест НЕ перебирает все возможные комбинации, а только некоторые.
   * Такой относительно полный перебор происходит автоматически при
   * тестировании индексов и курсоров. */
  fpta_column_set def;
  fpta_column_set_init(&def);
  EXPECT_NE(FPTA_SUCCESS, fpta_column_set_validate(&def));

  EXPECT_EQ(FPTA_EINVAL,
            fpta_column_describe("", fptu_cstr, fpta_primary_unique, &def));
  EXPECT_EQ(FPTA_EINVAL, fpta_column_describe("column_a", fptu_cstr,
                                              fpta_primary_unique, nullptr));

  EXPECT_EQ(FPTA_EINVAL,
            fpta_column_describe("column_a", fptu_uint64,
                                 fpta_primary_unique_reversed, &def));
  EXPECT_EQ(FPTA_EINVAL, fpta_column_describe("column_a", fptu_null,
                                              fpta_primary_unique, &def));

  /* Валидны все комбинации, в которых установлен хотя-бы один из флажков
   * fpta_index_fordered или fpta_index_fobverse. Другим словами,
   * не может быть unordered индекса со сравнением ключей
   * в обратном порядке. Однако, также допустим fpta_index_none.
   *
   * Поэтому внутри диапазона остаются только две недопустимые комбинации,
   * которые и проверяем. */
  EXPECT_EQ(FPTA_EINVAL, fpta_column_describe("column_a", fptu_cstr,
                                              fpta_index_funique, &def));
  EXPECT_EQ(FPTA_EINVAL,
            fpta_column_describe(
                "column_a", fptu_cstr,
                (fpta_index_type)(fpta_index_fsecondary | fpta_index_funique),
                &def));
  EXPECT_EQ(FPTA_EINVAL, fpta_column_describe("column_a", fptu_cstr,
                                              (fpta_index_type)-1, &def));
  EXPECT_EQ(
      FPTA_EINVAL,
      fpta_column_describe(
          "column_a", fptu_cstr,
          (fpta_index_type)(fpta_index_funique + fpta_index_fordered +
                            fpta_index_fobverse + fpta_index_fsecondary + 1),
          &def));

  EXPECT_EQ(FPTA_OK, fpta_column_describe("column_a", fptu_cstr,
                                          fpta_primary_unique, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));

  EXPECT_EQ(EEXIST, fpta_column_describe("column_b", fptu_cstr,
                                         fpta_primary_unique, &def));
  EXPECT_EQ(EEXIST,
            fpta_column_describe("column_a", fptu_cstr, fpta_secondary, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));
  EXPECT_EQ(FPTA_OK,
            fpta_column_describe("column_b", fptu_cstr, fpta_secondary, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));

  EXPECT_EQ(EEXIST,
            fpta_column_describe("column_b", fptu_fp64, fpta_secondary, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));
  EXPECT_EQ(FPTA_OK, fpta_column_describe("column_c", fptu_uint16,
                                          fpta_secondary, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));
}

TEST(Schema, Base) {
  /* Базовый тест создания таблицы.
   *
   * Сценарий:
   *  - открываем базу в режиме неизменяемой схемы и пробуем начать
   *    транзакцию уровня изменения схемы.
   *  - создаем и заполняем описание колонок.
   *  - создаем таблицу по сформированному описанию колонок.
   *  - затем в другой транзакции проверяем, что у созданной таблицы
   *    есть соответствующие колонки.
   *  - после в другой транзакции удаляем созданную таблицу,
   *    а также пробуем удалить несуществующую.
   *
   * Тест НЕ перебирает комбинации. Некий относительно полный перебор
   * происходит автоматически при тестировании индексов и курсоров. */
  ASSERT_TRUE(unlink(testdb_name) == 0 || errno == ENOENT);
  ASSERT_TRUE(unlink(testdb_name_lck) == 0 || errno == ENOENT);

  fpta_db *db = nullptr;
  EXPECT_EQ(FPTA_SUCCESS,
            fpta_db_open(testdb_name, fpta_async, 0644, 1, false, &db));
  ASSERT_NE(nullptr, db);

  fpta_txn *txn = (fpta_txn *)&txn;
  EXPECT_EQ(EPERM, fpta_transaction_begin(db, fpta_schema, &txn));
  ASSERT_EQ(nullptr, txn);
  ASSERT_EQ(FPTA_SUCCESS, fpta_db_close(db));

  //------------------------------------------------------------------------

  EXPECT_EQ(FPTA_SUCCESS,
            fpta_db_open(testdb_name, fpta_async, 0644, 1, true, &db));
  ASSERT_NE(nullptr, db);

  fpta_column_set def;
  fpta_column_set_init(&def);

  EXPECT_EQ(FPTA_OK, fpta_column_describe("pk_str_uniq", fptu_cstr,
                                          fpta_primary_unique, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_describe("first_uint", fptu_uint64,
                                          fpta_index_none, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_describe("second_fp", fptu_fp64,
                                          fpta_index_none, &def));
  EXPECT_EQ(FPTA_OK, fpta_column_set_validate(&def));

  //------------------------------------------------------------------------

  EXPECT_EQ(FPTA_EINVAL, fpta_transaction_begin(db, fpta_read, nullptr));
  EXPECT_EQ(FPTA_EINVAL, fpta_transaction_begin(db, (fpta_level)0, &txn));
  EXPECT_EQ(nullptr, txn);
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_schema, &txn));
  ASSERT_NE(nullptr, txn);

  EXPECT_EQ(FPTA_OK, fpta_table_create(txn, "table_1", &def));

  fpta_schema_info schema_info;
  EXPECT_EQ(FPTA_OK, fpta_schema_fetch(txn, &schema_info));
  EXPECT_EQ(1, schema_info.tables_count);

  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  txn = nullptr;

  //------------------------------------------------------------------------
  fpta_name table, col_pk, col_a, col_b, probe_get;

  fpta_pollute(&table, sizeof(table), 0); // чтобы valrind не ругался
  EXPECT_GT(0, fpta_table_column_count(&table));
  EXPECT_EQ(FPTA_EINVAL, fpta_table_column_get(&table, 0, &probe_get));

  EXPECT_EQ(FPTA_OK, fpta_table_init(&table, "tAbLe_1"));
  EXPECT_EQ(FPTA_OK, fpta_table_init(&table, "table_1"));
  EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_pk, "pk_str_uniq"));
  EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_a, "First_Uint"));
  EXPECT_EQ(FPTA_OK, fpta_column_init(&table, &col_b, "second_FP"));

  EXPECT_GT(0, fpta_table_column_count(&table));
  EXPECT_EQ(FPTA_EINVAL, fpta_table_column_get(&table, 0, &probe_get));

  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_read, &txn));
  ASSERT_NE(nullptr, txn);

  EXPECT_EQ(FPTA_OK, fpta_name_refresh_couple(txn, &table, &col_pk));
  EXPECT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_a));
  EXPECT_EQ(FPTA_OK, fpta_name_refresh(txn, &col_b));

  EXPECT_EQ(3, fpta_table_column_count(&table));
  EXPECT_EQ(FPTA_OK, fpta_table_column_get(&table, 0, &probe_get));
  EXPECT_EQ(0, memcmp(&probe_get, &col_pk, sizeof(fpta_name)));
  EXPECT_EQ(FPTA_OK, fpta_table_column_get(&table, 1, &probe_get));
  EXPECT_EQ(0, memcmp(&probe_get, &col_a, sizeof(fpta_name)));
  EXPECT_EQ(FPTA_OK, fpta_table_column_get(&table, 2, &probe_get));
  EXPECT_EQ(0, memcmp(&probe_get, &col_b, sizeof(fpta_name)));
  EXPECT_EQ(FPTA_EINVAL, fpta_table_column_get(&table, 3, &probe_get));

  EXPECT_EQ(fptu_cstr, fpta_shove2type(col_pk.shove));
  EXPECT_EQ(fpta_primary_unique, fpta_name_colindex(&col_pk));
  EXPECT_EQ(fptu_cstr, fpta_name_coltype(&col_pk));
  EXPECT_EQ(0, col_pk.column.num);

  EXPECT_EQ(fptu_uint64, fpta_shove2type(col_a.shove));
  EXPECT_EQ(fpta_index_none, fpta_name_colindex(&col_a));
  EXPECT_EQ(fptu_uint64, fpta_name_coltype(&col_a));
  EXPECT_EQ(1, col_a.column.num);

  EXPECT_EQ(fptu_fp64, fpta_shove2type(col_b.shove));
  EXPECT_EQ(fpta_index_none, fpta_name_colindex(&col_b));
  EXPECT_EQ(fptu_fp64, fpta_name_coltype(&col_b));
  EXPECT_EQ(2, col_b.column.num);

  EXPECT_EQ(FPTA_OK, fpta_schema_fetch(txn, &schema_info));
  EXPECT_EQ(1, schema_info.tables_count);
  EXPECT_EQ(FPTA_OK, fpta_name_refresh(txn, &schema_info.tables_names[0]));
  fpta_name_destroy(&schema_info.tables_names[0]);

  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn, false));
  txn = nullptr;

  // разрушаем привязанные идентификаторы
  fpta_name_destroy(&table);
  fpta_name_destroy(&col_pk);

  //------------------------------------------------------------------------
  EXPECT_EQ(FPTA_OK, fpta_transaction_begin(db, fpta_schema, &txn));
  ASSERT_NE(nullptr, txn);

  EXPECT_EQ(FPTA_OK, fpta_schema_fetch(txn, &schema_info));
  EXPECT_EQ(1, schema_info.tables_count);

  EXPECT_EQ(FPTA_OK, fpta_table_drop(txn, "Table_1"));
  EXPECT_EQ(FPTA_OK, fpta_schema_fetch(txn, &schema_info));
  EXPECT_EQ(0, schema_info.tables_count);

  EXPECT_EQ(MDB_NOTFOUND, fpta_table_drop(txn, "table_xyz"));
  EXPECT_EQ(FPTA_OK, fpta_schema_fetch(txn, &schema_info));
  EXPECT_EQ(0, schema_info.tables_count);

  EXPECT_EQ(FPTA_OK, fpta_transaction_end(txn, false));

  //------------------------------------------------------------------------
  EXPECT_EQ(FPTA_SUCCESS, fpta_db_close(db));
  // пока не удялем файлы чтобы можно было посмотреть и натравить mdbx_chk
  if (false) {
    ASSERT_TRUE(unlink(testdb_name) == 0);
    ASSERT_TRUE(unlink(testdb_name_lck) == 0);
  }
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
