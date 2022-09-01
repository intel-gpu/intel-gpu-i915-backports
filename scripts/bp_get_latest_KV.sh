#!/bin/bash
#	Get the latest supported KV from versions file
#

declare -a array1
declare -a array2
TEMP=`cat versions | grep ^RHEL_ | cut -f 1 -d "\"" | cut -d "_" -f 2`
array1=($TEMP)
readarray -t array2 < <(printf '%s\n' "${array1[@]}" | sort -rV)
printf "${array2[0]}\n"
