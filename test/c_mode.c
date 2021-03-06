﻿/*
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

/* test conformance for C mode */
#include <fast_positive/tables.h>

#ifdef _MSC_VER
#pragma warning(                                                               \
    disable : 4710 /* sprintf_s(char *const, const std::size_t, const char *const, ...): функция не является встроенной */)
#pragma warning(                                                               \
    disable : 4711 /* function 'fptu_init' selected for automatic inline expansion*/)
#endif /* windows mustdie */

#include <stdio.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  printf("\nno Windows, no Java, no Problems ;)\n");
  return 0;
}
