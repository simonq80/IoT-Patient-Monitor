#!/bin/bash

# load zephyr env 
source ../../zephyr-env.sh;

# reset nRF52
nrfjprog --eraseall;
nrfjprog --reset --program outdir/nrf52_pca10040/zephyr.hex -f nrf52;

# install the lastest binary
make BOARD=nrf52_pca10040 CONF_FILE=prj_cs7ns2.conf;
