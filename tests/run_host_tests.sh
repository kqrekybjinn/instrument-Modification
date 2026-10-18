#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
python3 tests/brake_frame_test.py
task_test_dir=$(mktemp -d)
trap 'rm -f "$task_test_dir/position" "$task_test_dir/workflow" "$task_test_dir/control" "$task_test_dir/log" "$task_test_dir/phase"; rmdir "$task_test_dir"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -Icomponents/app/include tests/position_math_test.cpp -o "$task_test_dir/position"
"$task_test_dir/position"
c++ -std=c++17 -Wall -Wextra -Werror -Icomponents/app/include tests/phase_position_test.cpp -o "$task_test_dir/phase"
"$task_test_dir/phase"
c++ -std=c++17 -Wall -Wextra -Werror -Icomponents/app/include tests/workflow_test.cpp -o "$task_test_dir/workflow"
"$task_test_dir/workflow"
c++ -std=c++17 -DMODI_HOST_TEST -Wall -Wextra -Werror -Itests/control_host/stubs -Icomponents/app/include -Icomponents/bsp/include tests/control_host/control_test.cpp components/app/src/app_control_service.cpp -o "$task_test_dir/control"
"$task_test_dir/control"
c++ -std=c++17 -DMODI_HOST_TEST -Wall -Wextra -Werror -Itests/log_host/stubs -Itests/control_host/stubs -Icomponents/app/include -Icomponents/bsp/include tests/log_host/log_test.cpp components/app/src/app_usb_log_service.cpp -o "$task_test_dir/log"
"$task_test_dir/log"
cmake -S tests/hmi_host -B build/hmi-host -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build/hmi-host --target hmi_render -j8 >/dev/null
build/hmi-host/hmi_render
