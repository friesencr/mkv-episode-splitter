#!/bin/bash
if [ -n "$1" ]; then
    "/home/chris/dev/mkv-splitter/build/mkvsplit-cli" -I "$1"
else
    "/home/chris/dev/mkv-splitter/build/mkvsplit-cli"
fi
