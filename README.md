# nightblitz

git://zxcvx.cc/repos/nightblitz.git

# Use

`insmod $(prefix)/ntbz-relay.ko drives_num=1 pd=/dev/name` will act as the relay.
`insmod $(prefix)/ntbz-relay.ko drives_num=2 pd=/dev/name1,/dev/name2` will distribute data in RAID 0 format.
Passing `rlvl=1` will use RAID 1 format (default is 0).
