#!/bin/bash

FILE=$1
if [[ -z "$FILE" ]]; then
        echo "Usage: $0 <job file>"
        exit
fi

pids=""
while read line; do
        taskset -c 0-7 ./fibonacci $line &
        pids="$pids $!"
done <<< $(cat $FILE)

echo $pids
wait $pids

