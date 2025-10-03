#!/bin/bash
# This script runs multiple applications in the background.
# It waits for all of them to finish before exiting.
./app A 5 &
./app B 10 &
./app C 4 &
./app D 2 &
./app E 3 &
./app F 15 &