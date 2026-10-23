#!/bin/sh
# board-side memory sampler: 60 samples, 30s apart, to /root/mw.log
: > /root/mw.log
i=1
while [ $i -le 60 ]; do
    echo "--- $i ---" >> /root/mw.log
    grep -E "MemFree|MemAvailable|SwapFree|AnonPages|Slab" /proc/meminfo >> /root/mw.log
    ps -eo pid,rss,comm --sort=-rss | head -6 >> /root/mw.log
    sleep 30
    i=$((i+1))
done
echo MW_ALL_DONE >> /root/mw.log
