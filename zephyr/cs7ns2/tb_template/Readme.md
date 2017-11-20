# CS7NS2-Zephyr Example: tb_temnplate

This example communicates with a thingsboard.io instance to (i) notify changes in the state of the four pushbuttons on the nRF52-DK and (ii) provide remote control of the four LEDs.


## MQTT

Communication of button state and remote control of the LEDs is implemented using the MQTT protocol.

Changes in button state are communicated as thingsboard.io telemetry messages. The updated telemetry (state) for a button takes the form a JSON message, e.g., `{"btn1": true}`. You could publish the updated state for several buttons in the same update (e.g. `{"btn1": true, "btn3": false}`) but this example publishes each state change independently for clarity.

Remote control of the four LEDs is implemented using the [thingsboard.io RPC mechanism](https://thingsboard.io/docs/reference/mqtt-api/#rpc-api). The nRF52-DK device subscribes to the thingsboard.io topic `v1/devices/me/rpc/request/+`, after which it will receive MQTT publish messages from thingsboard.io similar to the following:

```JSON
{
    "method": "putLights",
    "params": {
        "ledno": 0,
        "value": true
    }
}
```

In addition to publishing telemetry and subscribing to RPC publish messages, the example publishes occasional (every 15 seconds) attribute updates, just for illustration.


## Source Code

### main.c

[main.c](/cs7ns2/tb_template/src/main.c)

Waits for network initialisation to complete before initialising the remainder of the application. Network initialisation will only complete when you `connect` the other end of the BLE connection.

After completing the initialisation, the main thread enters an infinite loop, sleeping for an interval before causing a thingsboard.io attribute update to be published.

### tb_pubsub.h, tb_pubsub.c

[tb_pubsub.h](/cs7ns2/tb_template/src/tb_pubsub.h), [tb_pubsub.c](/cs7ns2/tb_template/src/tb_pubsub.c)

Implements all communication with the remote thingsboard.io instance. The connection to thingsboard.io is established and maintained by `pubsub_thread`, executing in its own thread instance. After establishing a connection, an MQTT subscription request is issued for the RPC topic (i.e. `v1/devices/me/rpc/request/+`). Next, the thread enters an infinite loop that blocks waiting for publish messages.

Other parts of the application cause attribute and telemetry updates to be published by initialising `pub_msg` descriptors and sending them to the `msgq` Zephyr message queue. A `pub_sub` descriptor can represent either an attribute or a telemetry update for thingsboard.io and this is determined by the MQTT topic to which the message is published (i.e. `v1/devices/me/attributes` for attributes and `v1/devices/me/telemetry` for telemetry.)

The `pub_sub` descriptors are initialised by the `tb_publish_attributes` and `tb_publish_attributes` functions provided by [tb_pubsub.c](/cs7ns2/tb_template/src/tb_pubsub.c). These can me called by other parts of the application, which then only need to be concerned with the proper formatting of the application-specific JSON data.

[tb_pubsub.c](/cs7ns2/tb_template/src/tb_pubsub.c) allocates memory blocks to hold the JSON data for MQTT publish messages from a Zephyr kernel memory pool. The memory is allocated when the `pub_sub` descriptor is being initialised (in the `prepare_msg` function) and then freed after the message has been sent (by `pubsub_thread`).


### sensors.h, sensors.c

[sensors.h](/cs7ns2/tb_template/src/sensors.h), [sensors.c](/cs7ns2/tb_template/src/sensors.c)

Captures nRF52-DK pushbutton state changes (both button press and button release).

State changes are detected using interrupts, which are handled by `btn_handler` but the actual work of publishing the update is deferred to `btn_alert_handler` using Zephyr kernel alerts. (This is an example of deferred interrupt handling and it allows us to perform the lengthy operation of publishing the MQTT telemetry messages arising from button presses in the context of a thread rather than an interrupt handler. In this case, we are making use of the Zephyr internal system workqueue thread.)

The `tb_publish_telemetry` function provided by [tb_pubsub.c](/cs7ns2/tb_template/src/tb_pubsub.c) is called to publish the telemetry update but the `btn_alert_handler` function is responsible for formulating the application-specific JSON data (e.g. `{"btn1": false}`).


### lights.h, lights.c

[lights.h](/cs7ns2/tb_template/src/lights.h), [lights.c](/cs7ns2/tb_template/src/lights.c)

A simple `putLights` function is provided to control individual LEDs in the nRF52-DK, in response to thingsboard.io RPC calls, arriving through [tb_pubsub.c](/cs7ns2/tb_template/src/tb_pubsub.c). The implementation follows the same pattern as the [blinky example](/cs7ns2/blinky/src/main.c).


## Useful Documentation
- [json.h](/include/json.h)
- [MQTT Standard](http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)


## Using the Example

It is assumed that you have followed the instructions for [getting started with Zephyr](https://gitlab.scss.tcd.ie/jdukes/cs7ns2/wikis/zephyr_getting_started.md) and that you [have the tb_demo example working](https://gitlab.scss.tcd.ie/jdukes/cs7ns2/wikis/thingsboard_demo.md).


### ACCESS TOKEN

**This tb_template example requires an important change in the way you configure devices in thingsboard.io**. You need to manually specify the **ACCESS_TOKEN** used by thingsboard.io to identify each device that you create. Rather than using randomly generated tokens, which you then need to hard-code into your nRF52 firmware, this example assumes the Bluetooth address of the device will be used as the ACCESS_TOKEN. In your thingsboard.io instance, select "Devices", your device and "Manage Credentials" and set the ACCESS TOKEN to the BLE address of your device (obtained using `hcitool lescan`.)


### Observing push Button State

You can use the thingsboard.io device telemetry tab to view the current state of each pushbutton or you can create a dashboard widget to do this.


### Testing RPC Light Control

To control one of the LEDs, create a dashboard switch widget or similar configured as follows in the ADVANCED tab:
- Set retrieve on/off value to none (Don't retrieve)
- Set Attribute/Value timeseries key to `value`
- You can leave the RPC get value method blank (although you may wish to implement this RPC call in your application)
- Set the ROC set value method to `putLights`
- Set the second JavaScript box for the "convert value function" to `return {"ledno":0, "value":value}`
- Test by connecting your device and toggling the widget to control the LED.
- Create additional widgets for the other LEDs, changing the `ledno` parameter to a value between 0 and 3 to select the LED.


## Tips

- When publishing telemetry or attributes from the device, error code -9 can result when your MQTT publish message is too large. A quick fix to avoid this is to use shorter attribute names (e.g. "`btn1` instead of `button1`").
