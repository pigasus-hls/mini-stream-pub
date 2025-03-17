all:
	make -i mini_sim

TARGET = de10_agilex:B2E2_8GBx4
#TARGET = $$OFS_BSP_BASE:ofs_n6001_usm

MHZ = 500MHz

SEED = 0

EARLY = -fsycl-link=early

EXTRA = -DSYCLBUILD

SIMULATE = -DFPGA_EMULATOR=1 -fsycl -fintelfpga -qactypes -Wall -Wextra -g -O0 #-O3

LINK = -fsycl -fintelfpga -qactypes -DFPGA_HARDWARE=1 -Xstarget=$(TARGET) -DIS_BSP -O3

SYNTHESIS = $(LINK) -Xsv -Xshardware -Xsclock=$(MHZ) -Xsseed=$(SEED)

RTLSIM = -Xsv -fsycl -fintelfpga -DFPGA_SIMULATOR=1 -Xssimulation -Xstarget=$(TARGET) -DIS_BSP -O3 -Xsghdl=3

############# build ##################

SRC = main.cpp

mini_sim:
	# building C emulation for debug
	icpx $(EXTRA) $(SIMULATE) $(SRC) -o build/mini_sim
	build/mini_sim

mini_rtl:
	icpx $(EXTRA) $(RTLSIM) $(SRC) -reuse-exe=build/mini_rtl -o build/mini_rtl

mini_link:
	# this takes minutes to get C to RTL report in look in build/min_fpga_.prj/reports
	icpx $(EXTRA) $(SYNTHESIS) $(EARLY) $(SRC) -o build/mini_hw

mini_fpga:
	# this takes 1.5 hours; the run should take 5msec
	icpx $(EXTRA) $(SYNTHESIS) $(SRC) -reuse-exe=build/mini_fpga -o build/mini_fpga
	build/mini_fpga

############# misc ##################

clang:
	clang-format -i *.cpp *.h

clean:
	rm -f *#*#* *~ */*~ 

reset:
	ofs_init
