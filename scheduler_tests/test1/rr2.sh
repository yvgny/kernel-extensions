#!/bin/sh

echo 35 > /proc/sys/kernel/sched_dummy_timeslice
echo 35 > /proc/sys/kernel/sched_dummy_age_threshold

nice -n 15 ../loop/loop A &
nice -n 15 ../loop/loop B &
nice -n 15 ../loop/loop C &

wait
echo 'done'

