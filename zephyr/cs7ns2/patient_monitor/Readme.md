# CS7NS2 Application: Patient Monitor

## Mac Addresses:
1. Ciaran: `EE:EF:3A:10:95:27`
2. Dan: `C7:44:81:6B:25:41`
3. Paddy:`DA:DA:10:B0:D2:05`
4. Simon: `CA:65:AF:2A:27:88`

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


* Easy flash

```
bash flash.sh
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

* Start thingsboard

```
sudo service thingsboard start
```

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
$ echo "connect C7:44:81:6B:25:41 2" > /sys/kernel/debug/bluetooth/6lowpan_control
$ echo "connect DA:DA:10:B0:D2:05 2" > /sys/kernel/debug/bluetooth/6lowpan_control
$ echo "connect CA:65:AF:2A:27:88 2" > /sys/kernel/debug/bluetooth/6lowpan_control
```

* List active connections

```
$ hcitool con
```

* Remove bluetooth device

```
$ hcitool ledc 64
```


* Run JS actuation

```
$ node install
$ node actuate_simulator.js
```

* ensure that the correct configurations are set `js/config.js`
and within `actuate_simulator.js`
