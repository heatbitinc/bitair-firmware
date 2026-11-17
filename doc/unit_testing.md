## Unit Testing
The firmware includes unit tests that run on the target (ESP32-S3).
Tests are located in the `test` subdirectory of a component (e.g. `components/stratum/test`).

For more information on unit testing with the ESP32-S3, see https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/unit-tests.html.

There are two test applications with the same set of tests:

- `test-ci` runs every test once, prints the summary and exits;
- `test` runs every test and then opens the interactive Unity menu.

### Building
From the repository root (ESP-IDF v5.5.4 environment):
```
idf.py -C test-ci -B build-test -D IDF_TARGET=esp32s3 build
```

### Flashing and running
Write only the test application at `0x10000`. The device keeps its bootloader, partition table and NVS (device
identity and settings), so flashing the firmware again afterwards restores it:
```
python -m esptool --chip esp32s3 -p /dev/ttyACM0 write_flash 0x10000 build-test/unit_test_stratum.bin
idf.py -p /dev/ttyACM0 monitor
```
The run ends with a line like `62 Tests 0 Failures 0 Ignored` (`CTRL-]` stops the monitor).

Never erase the whole flash of a real device: NVS holds its identity on the backend.

### Adding a Unit Test
In the following example, a unit test is added to the `foo` component.

```
$ ls -R components/foo
components/foo:
CMakeLists.txt  foo.c  include

components/foo/include:
foo.h
```

components/foo/CMakeLists.txt:
```
idf_component_register(
SRCS
    "foo.c"

INCLUDE_DIRS
    "include"

REQUIRES
)
```

components/foo/include/foo.h:
```c
#ifndef FOO_H
#define FOO_H

int foo(int i);

#endif // FOO_H
```

components/foo/foo.c:
```c
#include "foo.h"

int foo(int i) {
    return i;
}
```

A directory `components/foo/test` is created, with its test file and `CMakeLists.txt`

components/foo/test/CMakeLists.txt:
```
idf_component_register(SRC_DIRS "."
                    INCLUDE_DIRS "."
                    REQUIRES unity foo)

```

components/foo/test/test_foo.c:
```c
#include "unity.h"
#include "foo.h"

TEST_CASE("Foo returns what is provided", "[foo]")
{
    TEST_ASSERT_EQUAL_INT(42, foo(42));
}

```

The unit test application's `test/CMakeLists.txt` is modified to include `foo` in the test binary:
```diff
-set(TEST_COMPONENTS "bm1397 stratum" CACHE STRING "List of components to test")
+set(TEST_COMPONENTS "bm1397 stratum foo" CACHE STRING "List of components to test")
```

Build, flash, and monitor the test binary. Output from the new test should be present.
```
#### Running all the registered tests #####

Running Foo returns what is provided...
/home/dev/myrepos/ESP-Miner/components/foo/test/test_foo.c:4:Foo returns what is provided:PASS
...
```

### MQTT share reject interval tests

The `heatbit` tests cover interval counts, successful snapshot commits, rejects
arriving during a publish, unsent snapshots, overflow, and JSON escaping. They
are included in the default test component list and can also run natively using
the existing ESP-IDF Unity and cJSON sources:

```bash
cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations \
  -fsanitize=address,undefined \
  -Icomponents/heatbit \
  -I"$IDF_PATH/components/json/cJSON" \
  -I"$IDF_PATH/components/unity/unity/src" \
  components/heatbit/share_reject_stats.c \
  components/heatbit/test/test_share_reject_stats.c \
  "$IDF_PATH/components/json/cJSON/cJSON.c" \
  "$IDF_PATH/components/unity/unity/src/unity.c" \
  -o /tmp/esp-miner-share-reject-tests
/tmp/esp-miner-share-reject-tests
rm /tmp/esp-miner-share-reject-tests
```

Run this command from the repository root. These tests do not contact an MQTT
broker; broker delivery and ESP32 task scheduling require device validation.

