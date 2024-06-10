# To build against other version you can:
# make lnk_linux=lnk/lib/modules/5.10.217/kernel bld_linux=bld-v5.10 install
# in packs dir.

KVER1 = 6
KVER2 = 9
KVER3 = 3

lnk_linux = lnk/lib/modules/6.9.3/kernel

bld_linux = bld-v6.9
bld_ntbz = bld

dst_linux = $(hub)/packs/linux
dst_ntbz = $(hub)/packs/$(name)
src_ntbz = $(hub)/repos/$(name)

stackable:
	$(MAKE) -j $(nproc)		\
		-C /$(dst_linux)/$(bld_linux)/	\
		M=/$(dst_ntbz)/$(bld_ntbz)/	\
		src=/$(src_ntbz)/		\
		modules

kbuild: stackable

install: kbuild
	mkdir -p /$(dst_ntbz)/lnk
	cp -u /$(dst_ntbz)/bld/*.ko /$(dst_ntbz)/lnk/
	mkdir -p /$(dst_linux)/$(lnk_linux)/drivers/ntbz/	# for gdb
	cp -u /$(dst_ntbz)/$(bld_ntbz)/*.ko /$(dst_linux)/$(lnk_linux)/drivers/ntbz/

clean:
	find /$(dst_ntbz)/ -type f -not \( -name 'Makefile' -o -name 'qtxpkg' \) -delete
