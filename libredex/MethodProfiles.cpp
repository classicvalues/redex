/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodProfiles.h"

#include <iostream>
#include <stdio.h>
#include <stdlib.h>

#include "Timer.h"
#include "Trace.h"

using namespace method_profiles;

extern int errno;

bool MethodProfiles::parse_stats_file(const std::string& csv_filename) {
  Timer t("Parsing agg_method_stats_file");
  TRACE(METH_PROF, 3, "input csv filename: %s", csv_filename.c_str());
  if (csv_filename == "") {
    TRACE(METH_PROF, 2, "No csv file given");
    return false;
  }

  auto cleanup = [](FILE* fp, char* line = nullptr) {
    if (fp) {
      fclose(fp);
    }
    if (line) {
      // getline allocates buffer space and expects us to free it
      free(line);
    }
  };

  // Using C-style file reading and parsing because it's faster than the
  // iostreams equivalent and we expect to read very large csv files.
  FILE* fp = fopen(csv_filename.c_str(), "r");
  if (fp == nullptr) {
    std::cerr << "FAILED to open " << csv_filename << ": " << strerror(errno)
              << "\n";
    cleanup(fp);
    return false;
  }

  // getline will allocate a buffer and put a pointer to it here
  char* line = nullptr;
  size_t len = 0;
  bool first = true;
  while (getline(&line, &len, fp) != -1) {
    bool success = parse_line(line, first);
    if (!success) {
      cleanup(fp, line);
      return false;
    }
    first = false;
  }
  if (errno != 0) {
    std::cerr << "FAILED to read a line: " << strerror(errno) << "\n";
    cleanup(fp, line);
    return false;
  }

  TRACE(METH_PROF, 1, "MethodProfiles successfully parsed %zu rows",
        m_method_stats.size());
  cleanup(fp, line);
  return true;
}

bool MethodProfiles::parse_line(char* line, bool first) {
  if (first) {
    return parse_header(line);
  }

  auto parse_byte = [](const char* tok) -> uint8_t {
    char* rest = nullptr;
    const auto result = static_cast<uint8_t>(strtoul(tok, &rest, 10));
    const auto len = strlen(rest);
    always_assert_log(rest != tok, "can't parse %s into a uint8_t", tok);
    always_assert_log(len == 0 || (len == 1 && rest[0] == '\n'),
                      "can't parse %s into a uint8_t", tok);
    return result;
  };
  auto parse_double = [](const char* tok) -> double {
    char* rest = nullptr;
    const auto result = strtod(tok, &rest);
    const auto len = strlen(rest);
    always_assert_log(rest != tok, "can't parse %s into a double", tok);
    always_assert_log(len == 0 || (len == 1 && rest[0] == '\n'),
                      "can't parse %s into a double", tok);
    return result;
  };

  Stats stats;
  DexMethodRef* ref = nullptr;
  auto parse_cell = [&](char* tok, uint32_t i) -> bool {
    switch (i) {
    case INDEX:
      // Don't need this raw data. It's an arbitrary index (the line number in
      // the file)
      return true;
    case NAME:
      ref = DexMethod::get_method(tok);
      if (ref == nullptr) {
        TRACE(METH_PROF, 4, "failed to resolve %s", tok);
      }
      return true;
    case APPEAR100:
      stats.appear_percent = parse_double(tok);
      return true;
    case APPEAR_NUMBER:
      // Don't need this raw data. appear_percent is the same thing but
      // normalized
      return true;
    case AVG_CALL:
      stats.call_count = parse_double(tok);
      return true;
    case AVG_ORDER:
      // Don't need this raw data. order_percent is the same thing but
      // normalized
      return true;
    case AVG_RANK100:
      stats.order_percent = parse_double(tok);
      return true;
    case MIN_API_LEVEL:
      stats.min_api_level = parse_byte(tok);
      return true;
    default:
      std::cerr << "FAILED to parse line. Too many columns\n";
      return false;
    }
  };

  bool success = parse_cells(line, parse_cell);
  if (!success) {
    return false;
  }
  if (ref != nullptr) {
    TRACE(METH_PROF, 4, "%s -> {%f, %f, %f, %u}", SHOW(ref),
          stats.appear_percent, stats.call_count, stats.order_percent,
          stats.min_api_level);
    m_method_stats.emplace(ref, stats);
  }
  return true;
}

bool MethodProfiles::parse_header(char* line) {
  auto check_cell = [](const char* expected, const char* tok,
                       uint32_t i) -> bool {
    const size_t MAX_CELL_LENGTH = 1000;
    if (strncmp(tok, expected, MAX_CELL_LENGTH) != 0) {
      std::cerr << "Unexpected Header (column " << i << "): " << tok
                << " != " << expected << "\n";
      return false;
    }
    return true;
  };
  auto parse_cell = [&](char* tok, uint32_t i) -> bool {
    switch (i) {
    case INDEX:
      return check_cell("index", tok, i);
    case NAME:
      return check_cell("name", tok, i);
    case APPEAR100:
      return check_cell("appear100", tok, i);
    case APPEAR_NUMBER:
      return check_cell("appear#", tok, i);
    case AVG_CALL:
      return check_cell("avg_call", tok, i);
    case AVG_ORDER:
      return check_cell("avg_order", tok, i);
    case AVG_RANK100:
      return check_cell("avg_rank100", tok, i);
    case MIN_API_LEVEL:
      return check_cell("min_api_level", tok, i);
    default:
      return check_cell("\n", tok, i);
    }
  };
  return parse_cells(line, parse_cell);
}
