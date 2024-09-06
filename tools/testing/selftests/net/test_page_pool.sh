#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2024 Yunsheng Lin <linyunsheng@huawei.com>
# Copyright (C) 2018 Uladzislau Rezki (Sony) <urezki@gmail.com>
#
# This is a test script for the kernel test driver to test the
# correctness and performance of page_pool's implementation.
# Therefore it is just a kernel module loader. You can specify
# and pass different parameters in order to:
#     a) analyse performance of page_pool;
#     b) stressing and stability check of page_pool subsystem.

DRIVER="./page_pool/page_pool_test.ko"
CPU_LIST=$(grep -m 2 processor /proc/cpuinfo | cut -d ' ' -f 2)
CPU_CNT=$(echo $CPU_LIST | wc -w)
TEST_CPU_0=$(echo $CPU_LIST | awk '{print $1}')

if [ $CPU_CNT -gt 1 ]; then
	TEST_CPU_1=$(echo $CPU_LIST | awk '{print $2}')
	NR_TEST=100000000
else
	TEST_CPU_1=$TEST_CPU_0
	NR_TEST=1000000
fi

# 1 if fails
exitcode=1

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

#
# Static templates for testing of page_pool APIs.
# Also it is possible to pass any supported parameters manually.
#
SMOKE_PARAM="test_push_cpu=$TEST_CPU_0 test_pop_cpu=$TEST_CPU_1"
NONFRAG_PARAM="$SMOKE_PARAM nr_test=$NR_TEST"
FRAG_PARAM="$NONFRAG_PARAM test_alloc_len=2048 test_frag=1"

check_test_requirements()
{
	uid=$(id -u)
	if [ $uid -ne 0 ]; then
		echo "$0: Must be run as root"
		exit $ksft_skip
	fi

	if ! which insmod > /dev/null 2>&1; then
		echo "$0: You need insmod installed"
		exit $ksft_skip
	fi

	if [ ! -f $DRIVER ]; then
		echo "$0: You need to compile page_pool_test module"
		exit $ksft_skip
	fi
}

run_nonfrag_check()
{
	echo "Run performance tests to evaluate how fast nonaligned alloc API is."

	insmod $DRIVER $NONFRAG_PARAM > /dev/null 2>&1
	echo "Done."
	echo "Check the kernel ring buffer to see the summary."
}

run_frag_check()
{
	echo "Run performance tests to evaluate how fast aligned alloc API is."

	insmod $DRIVER $FRAG_PARAM > /dev/null 2>&1
	echo "Done."
	echo "Check the kernel ring buffer to see the summary."
}

run_smoke_check()
{
	echo "Run smoke test."

	insmod $DRIVER $SMOKE_PARAM > /dev/null 2>&1
	echo "Done."
	echo "Check the kernel ring buffer to see the summary."
}

function run_test()
{
	if [ $CPU_CNT -gt 1 ]; then
		run_smoke_check
		run_nonfrag_check
		run_frag_check
	fi
}

check_test_requirements
run_test

exit 0
