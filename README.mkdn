Ratibor 4.19.x Kernel Source based on KnightWalker by RyuujiX
====================================

OverClock Kernel
------------------
This Kernel is using Stock Freq. If u want Overclocked Freq, Do this:

To Overclock CPU:

- Add `overclock.cpu=1` into boot cmdline. Or
- Hardcode, set this to 1 https://github.com/SonicBSV/android_kernel_asus_sdm660-4.19/blob/lineage-21.0/drivers/clk/qcom/clk-cpu-osm-660.c#L263

To Overclock GPU:

- Add `overclock.gpu=1` into boot cmdline. Or
- Hardcode, set this to 1 https://github.com/SonicBSV/android_kernel_asus_sdm660-4.19/blob/lineage-21.0/drivers/gpu/msm/adreno.c#L31

KernelSU
--------
KernelSU is implemented and enabled by default. If u don't want it, Do this:

- Add `kernelsu.enabled=0` into boot cmdline, or
- Disable `CONFIG_KSU`

If u only want to enable Safe Mode (U should enable KernelSU too), Which u just want Root Access and Modules Feature disabled, Do this:

- Add `kernelsu.safemode=1` into boot cmdline. Or
- Set `CONFIG_KSU_SAFE_MODE=y` in defconfig.

Source: https://github.com/RyuujiX/KernelSU


Using New Novatek Touchscreen Driver
------------------------------------
This source is still using original driver from 4.4 source... but, i have updated nvt driver.

Working smoothly on mine with new driver.

If u want to use new driver, do this:

- Add `use_new_nvtouch=1` into boot cmdline. Or
- Hardcode, set this to 1 https://github.com/SonicBSV/android_kernel_asus_sdm660-4.19/blob/lineage-21.0/init/main.c#L145
- Or, Disable Original NVT Driver in defconfig. `CONFIG_TOUCHSCREEN_NT36xxx_ASUS` is New NVT Driver.
