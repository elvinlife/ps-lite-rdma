#!/bin/bash
# set -x
export PS_VERBOSE=2
if [ $# -lt 3 ]; then
    echo "usage: $0 num_servers num_workers bin [args..]"
    exit -1;
fi

export DMLC_NUM_SERVER=$1
shift
export DMLC_NUM_WORKER=$1
shift
bin=$1
shift
arg="$@"

# start workers
#export DMLC_PS_ROOT_URI='192.168.0.1'
export DMLC_INTERFACE=ib0
export DMLC_PS_ROOT_URI='10.0.0.7'
export DMLC_PS_ROOT_PORT=8000
export DMLC_ROLE='worker'
for ((i=0; i<${DMLC_NUM_WORKER}; ++i)); do
    export HEAPPROFILE=./W${i}
    ${bin} ${arg} &
done

wait
