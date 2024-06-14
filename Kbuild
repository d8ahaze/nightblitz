KVER1 = 6
KVER2 = 9
KVER3 = 3

ccflags-y += -save-temps
ccflags-y += -DKVER1=$(KVER1) -DKVER2=$(KVER2) -DKVER3=$(KVER3)

obj-m := ntbz-relay.o

ntbz-relay-objs := stackable/relay.o
