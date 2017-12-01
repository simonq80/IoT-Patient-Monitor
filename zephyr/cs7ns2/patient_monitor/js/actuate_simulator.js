var CONFIG = require('./config.json');

var HEART_RATE_DEVICE = 0;
var TEMPERATURE_DEVICE = 0;
var BASE_URL = CONFIG.TB_ADDRESS+":" + CONFIG.TB_PORT;
var NUMBER_OF_LEDS = 4;

// Set the state of the lights on the device `deviceId`
function doLights(deviceId, lightNo, state) {

    // Use the server-side device RPC API to cause thingsboard to issue a device
    // RPC to a device that we identify by `buttonEntityId`
    // See: https://thingsboard.io/docs/user-guide/rpc/

    var request = require("request");
    var url = "http://" + BASE_URL + "/api/plugins/rpc/oneway/" + deviceId;

    // The JSON RPC description must match that expected in tb_pubsub.c
    var req = {
        "method" : "putLights",
        "params" : {
            "ledno" : lightNo,
            "value" : state
        }
    };

    // Issue the HTTP POST request
    request({
        url: url,
        method: "POST",
        json: req,
        headers: {
            "X-Authorization": "Bearer " + CONFIG.TB_TOKEN,
            // Note the error in the TB docs: `Bearer` is missing from
            // `X-Authorization`, causing a 401 error response
        }
    }, function (error, response, body) {
        if (!error && response.statusCode === 200) {
            console.log("OK" + ((typeof body != 'undefined') ? ": " + body : ""));
        }
        else {
            console.log("error: " + error)
            console.log("response.statusCode: " + response.statusCode)
            console.log("response.statusText: " + response.statusText)
        }
    });
}

function doBuzzer(deviceId, state) {
  var request = require("request");
  var url = "http://" + BASE_URL + "/api/plugins/rpc/oneway/" + deviceId;

  var req = {
    "method" : "putBuzzer",
    "params" : {
      "value" : state
    }
  };

  // Issue the HTTP POST request
  request({
      url: url,
      method: "POST",
      json: req,
      headers: {
          "X-Authorization": "Bearer " + CONFIG.TB_TOKEN,
          // Note the error in the TB docs: `Bearer` is missing from
          // `X-Authorization`, causing a 401 error response
      }
  }, function (error, response, body) {
      if (!error && response.statusCode === 200) {
          console.log("OK" + ((typeof body != 'undefined') ? ": " + body : ""));
      }
      else {
          console.log("error: " + error)
          console.log("response.statusCode: " + response.statusCode)
          console.log("response.statusText: " + response.statusText)
      }
  });
}

// Process device telemetry updates received from thingsboard device `deviceId`
function processTelemetryData(deviceId, data) {

    // Note: Unfortunately the JSON parser gives us strings for the booleans
    // that we originally published from the tb_template device firmware. We
    // need to use string comparison to interpret them.

    console.log("Telemetry from " + deviceId + " : " + JSON.stringify(data));

    // Turn on lights if temperature falls below zero
    if (deviceId == CONFIG.DEVICE_IDS[TEMPERATURE_DEVICE]) {
      if (typeof data.tmp !== 'undefined') {
          var temperature = data.tmp[0][1];
          if(temperature < 0){
            for(var ledno = 0; ledno < NUMBER_OF_LEDS; ledno++ ){
              doLights(deviceId, ledno, true);
            }
          }
      }

      // Turn off lights by pressing any button
      if (typeof data.btn0 !== 'undefined') {
        for(var ledno = 0; ledno < NUMBER_OF_LEDS; ledno++ ){
          doLights(deviceId, ledno, false);
        }
      }

      if (typeof data.btn1 !== 'undefined') {
        doBuzzer(deviceId, false);
      }
    }

    // Heart rate actuation
    if (deviceId == CONFIG.DEVICE_IDS[HEART_RATE_DEVICE]) {
        if (typeof data.hrt !== 'undefined') {
          var heartRate = data.hrt[0][1];
          if(heartRate < 0){
            console.log("HEART RATE = " + heartRate);
          }
        }
    }


}


// Use the thingsboard Websocket API to subscribe to device telemetry updates
// See: https://thingsboard.io/docs/user-guide/telemetry/

var WebSocketClient = require('websocket').client;
var client = new WebSocketClient();

client.on('connectFailed', function(error) {
    console.log('Connect Error: ' + error.toString());
});

client.on('connect', function(connection) {

    console.log('WebSocket Client Connected');

    connection.on('error', function(error) {
        console.log("Connection Error: " + error.toString());
    });

    connection.on('close', function() {
        console.log('echo-protocol Connection Closed');
    });

    connection.on('message', function(message) {
        if (message.type === 'utf8') {
            var rxObj = JSON.parse(message.utf8Data);
            if (typeof rxObj.subscriptionId !== 'undefined') {
                processTelemetryData(CONFIG.DEVICE_IDS[rxObj.subscriptionId], rxObj.data);
            }
        }
    });

    // Subscribe to the latest telemetry from the device (specified by an index
    // into the DEVICE_IDS array)
    // See: https://thingsboard.io/docs/user-guide/telemetry/
    function subscribe(deviceIdx) {
        var req = {
            tsSubCmds: [
                {
                    entityType: "DEVICE",
                    entityId: CONFIG.DEVICE_IDS[deviceIdx],
                    scope: "LATEST_TELEMETRY",
                    cmdId: deviceIdx
                }
            ],
            historyCmds: [],
            attrSubCmds: []
        };

        console.log("Subscribing to " + CONFIG.DEVICE_IDS[deviceIdx]);
        connection.sendUTF(JSON.stringify(req));
    }

    if (connection.connected) {

        // Subscribe to telemetry updates for MY_BTN_LED_DEVICE
        subscribe(TEMPERATURE_DEVICE);

        // Or subscribe to all if you want
        // for (deviceIdx = 0; deviceIdx < DEVICE_IDS.length; deviceIdx++) {
        //     subscribe(deviceIdx);
        // }
    }
});

client.connect("ws://" + BASE_URL + "/api/ws/plugins/telemetry?token=" + CONFIG.TB_TOKEN);
