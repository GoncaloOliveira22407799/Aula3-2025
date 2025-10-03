#!/bin/bash
# This script runs multiple applications in the background.
# It waits for all of them to finish before exiting.
./app A 10 &
./app B 15 &
./app C 20 &
