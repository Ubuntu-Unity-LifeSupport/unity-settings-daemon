#!/bin/bash

MONITORS_FILES="monitors.xml monitors.xml.backup monitors-v1-backup.xml"
config_path="$HOME/.config"

if [ -n "$XDG_CONFIG_HOME" ]; then
    config_path="$XDG_CONFIG_HOME";
fi

for monitor_file in $MONITORS_FILES; do
    old_monitors="$config_path/$monitor_file"
    new_monitors="$config_path/unity-$monitor_file"

    if [ -f "$old_monitors" ]; then
        if grep '<monitors[^>]*>' "$old_monitors" | grep -Fq 'version="1"'; then
           cp -v "$old_monitors" "$new_monitors"
        fi
    fi
done

if [ -f "$config_path/unity-monitors-v1-backup.xml" ] &&
   [ ! -f "$config_path/unity-monitors.xml" ]; then
   cp -v "$config_path/unity-monitors-v1-backup.xml" \
         "$config_path/unity-monitors.xml"
fi
