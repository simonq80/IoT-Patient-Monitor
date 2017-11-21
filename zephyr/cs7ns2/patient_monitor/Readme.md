# CS7NS2 Application: Patient Monitor

## Preparing Code
Ubuntu:
```
$ sudo apt-get install git make gcc g++ ncurses-dev gperf ccache doxygen dfu-util device-tree-compiler python3-ply python3-pip
$ pip3 install --user -r scripts/requirements.txt
```

Macintosh:
```
$ brew install dfu-util doxygen qemu dtc python3 gperf
$ curl -O 'https://bootstrap.pypa.io/get-pip.py'
$ chmod +x get-pip.py
$ ./get-pip.py
$ rm get-pip.py
```

* Install dependencies

```
pip3 install --user -r scripts/requirements.txt
```


* Set environment variables

```
$ export GCCARMEMB_TOOLCHAIN_PATH=/Users/username/Developer/gcc-arm-none-eabi/gcc-arm-none-eabi-6-2017-q2-update/
$ export ZEPHYR_GCC_VARIANT=gccarmemb
```


```
$ cd zephyr
$ source zephyr-env.sh
```

[More help ...](https://gitlab.scss.tcd.ie/jdukes/cs7ns2/wikis/Zephyr_getting_started.md)

##  Create Zephyr Binary:

* Navigate to DIR

```
$ cd zephyr/cs7ns2/patient_monitor
```

* Build program

```
$ make BOARD=nrf52_pca10040 CONF_FILE=prj_cs7ns2.conf
```

* Erase nordic board

```
$ nrfjprog --eraseall
$ nrfjprog --reset --program outdir/nrf52_pca10040/zephyr.hex -f nrf52
```

[More help ...](https://gitlab.scss.tcd.ie/jdukes/cs7ns2/wikis/thingsboard_demo.md)


## Thingsboard VM Config

* Discover bluetooth device

```
$ sudo su -
$ modprobe bluetooth_6lowpan
$ echo 1 > /sys/kernel/debug/bluetooth/6lowpan_enable
$ hcitool lescan
```

* Connect mac address of desired devices

```
$ echo "connect EE:EF:3A:10:95:27 2" > /sys/kernel/debug/bluetooth/6lowpan_control
```

* List active connections

```
$ hcitool con
```

* Remove bluetooth device

```
$ hcitool ledc 64
```
