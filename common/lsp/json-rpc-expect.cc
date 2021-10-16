// Copyright 2021 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// A utility similar to expect(1) but for json-rpc.

#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>

#include "absl/status/status.h"
#include "common/lsp/message-stream-splitter.h"
#include "nlohmann/json.hpp"

#ifndef _WIN32
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
// Windows doesn't have Posix read(), but something called _read
#define read(fd, buf, size) _read(fd, buf, size)
#endif

using nlohmann::json;
using verible::lsp::MessageStreamSplitter;

static int usage(const char *progname, const char *msg) {
  fprintf(stderr, "%s\n\nUsage: %s <expect-script-file>\n", msg, progname);
  std::cerr << R"(
The program receives JSON-RPC header/body responses on stdin
and compares if the response is contained in the response array.
Right now, the responses are checked to arrive in the same sequence they
are mentioned in the array (this might change in the future if we consider
async responses).

The exit code will be 0 if all expected responses have been received or the
index (1-based) where they failed.

A typical expect-script file would be a json array like this
[
 {
   "json_contains": { ... some json, but only interesting fields ... }
 },
]
)";
  return 1;
}

static bool CheckNested(const json &expected, const json &received) {
  for (const auto &[key, value] : expected.items()) {
    auto found = received.find(key);
    if (found == received.end()) {
      std::cerr << "key '" << key << "' missing in " << received << std::endl;
      return false;
    }
    if (value.is_object()) {
      return CheckNested(value, *found);
    }
    if (value != *found) {
      std::cerr << key << ": expected: " << value << "; got: " << *found
                << std::endl;
      return false;
    }
  }
  return true;
}

static bool CheckExpectedMatch(const json &expected, const json &received) {
  auto json_contains = expected.find("json_contains");
  if (json_contains == expected.end()) {
    std::cerr << "'json_contains' key missing " << expected << std::endl;
    return false;
  }
  json partial_data = *json_contains;
  return CheckNested(partial_data, received);
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  static constexpr int kInFiledescriptor = 0;  // STDIN_FILENO on Unix

  if (argc != 2) {
    return usage(argv[0], "Required filename");
  }
  std::ifstream file(argv[1]);
  if (!file.is_open()) return usage(argv[0], "Could not open file");
  const std::string expect_script((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  const json expect_data = json::parse(expect_script);
  if (!expect_data.is_array()) {
    return usage(argv[0], "Input needs to be a json array");
  }

  // Let's be lenient in parsing.
  const bool kStrictCRLFrequirement = true;
  MessageStreamSplitter stream_splitter(4096, !kStrictCRLFrequirement);

  int first_error = -1;
  size_t expect_pos = 0;
  stream_splitter.SetMessageProcessor(
      [&expect_data, &expect_pos, &first_error](absl::string_view,
                                                absl::string_view body) {
        std::cerr << "Got: " << body << std::endl;
        json received = json::parse(body);
        json expected = expect_data[expect_pos];
        if (!CheckExpectedMatch(expected, received)) {
          if (first_error < 0) first_error = expect_pos;
        }
        ++expect_pos;
      });

  absl::Status status = absl::OkStatus();
  while (status.ok()) {
    status = stream_splitter.PullFrom([](char *buf, int size) -> int {  //
      return read(kInFiledescriptor, buf, size);
    });
  }

  if (status.code() != absl::StatusCode::kUnavailable) {
    std::cerr << "Expected EOF, got " << status << std::endl;
    return expect_pos;
  }

  return expect_pos == expect_data.size() ? (first_error + 1) : expect_pos;
}
