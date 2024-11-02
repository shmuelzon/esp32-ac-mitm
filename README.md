# ESP32-AC-MITM

This project is a man-in-the-middle for your air conditioner's IR remote
receiver. It's developed for the ESP32 SoC and is based on
[ESP-IDF](https://github.com/espressif/esp-idf) release v5.2.2.

This project can be used for controlling an AC, over MQTT, while making the
integration point simple and universal while also ensuring the AC state is
consistent as all remote commands go through the ESP32.

For this to work, you need to remove the AC's IR reciever from its current
controller, connect it to one of the ESP's and connect another ESP GPIO to the
existing controller's IR receiver pin. **Be sure to level shift, as needed!**


BLE to MQTT bridge, i.e. it exposes BLE GATT characteristics
as MQTT topics for bidirectional communication. 
Note that using any other ESP-IDF version might not be stable or even compile.

This project publishes the following MQTT topics:
* `AC-MITM-XXX/Action` - Current AC operation, one of: `fan`, `cooling`,
  `heating` or `drying`
* `AC-MITM-XXX/Fan` - Fan mode, one of: `auto`, `low`, `medium` or `high`
* `AC-MITM-XXX/Mode` - Operation mode, one of: `off`, `auto`, `cool`, `heat`,
  `dry` or `fan_only`
* `AC-MITM-XXX/Temperature` - The AC's set temperature
* `AC-MITM-XXX/Power` - Is the AC turned on, one of: `on` or `off`

In addition to AC control, the following book-keeping topics are also published:
* `AC-MITM-XXX/Version` - The BLE2MQTT application version currently running
* `AC-MITM-XXX/ConfigVersion` - The BLE2MQTT configuration version currently
  loaded (MD5 hash of configuration file)
* `AC-MITM-XXX/Uptime` - The uptime of the ESP32, in seconds, published every
  minute
* `AC-MITM-XXX/Status` - `Online` when running, `Offline` when powered off
  (the latter is an LWT message)

## Compiling

1. Install `ESP-IDF`

You will first need to install the
[Espressif IoT Development Framework](https://github.com/espressif/esp-idf).
The [Installation Instructions](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/linux-macos-setup.html)
have all of the details. Make sure to follow ALL the steps, up to and including
step 4 where you set up the tools and the `get_idf` alias.

2. Download the repository and its dependencies:

```bash
git clone --recursive https://github.com/shmuelzon/esp32-ac-mitm
```

3. Modify the config.json and flash

Modify the [configuration file](#configuration) to fit your environment, build
and flash (make sure to modify the serial device your ESP32 is connected to):

```bash
idf.py build flash
```

## Remote Logging

If configured, the application can send the logs remotely via UDP to another
host to allow receiving logs from remote devices without a serial connection.
To receive these logs on your host, execute `idf.py remote-monitor`.

## Configuration

The configuration file provided in located at
[data/config.json](data/config.json) in the repository. It contains all of the
different configuration options.

The `network` section should contain either a `wifi` section or an `eth`
section.  If case there are both, the `eth` section has preference over the
`wifi` section.

Optionally, the network section can contain a `hostname` which, if set,
is used in MQTT subscriptions as well. In such case, relace `BLE2MQTT-XXX` in
this documentation with the hostname you have set.

The `wifi` section below includes the following entries:
```json
{
  "network": {
    "hostname": "MY_HOSTNAME",
    "wifi": {
      "ssid": "MY_SSID",
      "password": "MY_PASSWORD",
      "eap": {
        "method": null,
        "identity": null,
        "client_cert": null,
        "client_key": null,
        "server_cert": null,
        "username": null,
        "password": null
      }
    }
  }
}
```
* `ssid` - The WiFi SSID the ESP32 should connect to
* `password` - The security password for the above network
* `eap` - WPA-Enterprise configuration (for enterprise networks only)
  * `method` - `TLS`, `PEAP` or `TTLS`
  * `identity` - The EAP identity
  * `ca_cert`, `client_cert`, `client_key` - Full path names, including a
    leading slash (/), of the certificate/key file (in PEM format) stored under
    the data folder
  * `username`, `password` - EAP login credentials

The `eth` section below includes the following entries:
```json
{
  "network": {
    "eth": {
      "phy": "MY_ETH_PHY",
      "phy_power_pin": -1
    }
  }
}
```
* `phy` - The PHY chip connected to ESP32 RMII, one of:
  * `IP101`
  * `RTL8201`
  * `LAN8720`
  * `DP83848`
* `phy_power_pin` - Some ESP32 Ethernet modules such as the Olimex ESP32-POE require a GPIO pin to be set high in order to enable the PHY. Omitting this configuration or setting it to -1 will disable this.

_Note: Defining the `eth` section will disable WiFi_

The `mqtt` section below includes the following entries:
```json
{
  "mqtt": {
    "server": {
      "host": "192.168.1.1",
      "port": 1883,
      "ssl": false,
      "client_cert": null,
      "client_key": null,
      "server_cert": null,
      "username": null,
      "password": null,
      "client_id": null
    },
    "publish": {
      "qos": 0,
      "retain": true
    }
  }
}
```
* `server` - MQTT connection parameters
  * `host` - Host name or IP address of the MQTT broker
  * `port` - TCP port of the MQTT broker. If not specificed will default to
    1883 or 8883, depending on SSL configuration
  * `client_cert`, `client_key`, `server_cert` - Full path names, including a
    leading slash (/), of the certificate/key file (in PEM format) stored under
    the data folder. For example, if a certificate file is placed at
    `data/certs/my_cert.pem`, the value stored in the configuration should be
    `/certs/my_cert.pem`
  * `username`, `password` - MQTT login credentials
  * `client_id` - The MQTT client ID
* `publish` - Configuration for publishing topics

The `ac` section of the configuration file includes the following configuration:
```json
{
  "ac": {
    "protocol": "",
    "ir_rx": {
      "gpio": -1,
      "inverted": false
    },
    "ir_tx": {
      "gpio": -1,
      "inverted": false
    },
    "is_on_gpio": -1
  }
}
```
* `protocol` - The name of the AC's protocol handler. Currently supported
  values: `airwell`
* `ir_rx` - The configuration of the IR receiver. The GPIO it's connected to and
  if the logic level should be inverted
* `ir_tx` - The configuration of the output signal. The GPIO that's connected
  back to the AC controller board, instead of the original IR receiver 
* `is_on_gpio` - If your air conditioner has a single button to toggle the AC's
  power, you can use this GPIO configuration so the application will always know
  if the AC is currently on or not and send the correct signal to the AC
  controller board

The optional `log` section below includes the following entries:
```json
{
  "log": {
    "host": "224.0.0.200",
    "port": 5000
  }
}
```
* `host` - The hostname or IP address to send the logs to. In case of an IP
  address, this may be a unicast, broadcast or multicast address
* `port` - The destination UDP port

## OTA

It is possible to upgrade both firmware and configuration file over-the-air once
an initial version was flashed via serial interface. To do so, execute:
`idf.py upload` or `idf.py upload-config` accordingly.
The above will upgrade all AC-MITM devices connected to the MQTT broker defined
in the configuration file. It is also possible to upgrade a specific device by
adding the `OTA_TARGET` variable to the above command set to the host name of
the requested device, e.g.:
```bash
OTA_TARGET=AC-MITM-XXXX idf.py upload
```

Note: In order to avoid unneeded upgrades, there is a mechanism in place to
compare the new version with the one that resides on the flash. For the firmware
image it's based on the git tag and for the configuration file it's an MD5 hash
of its contents. In order to force an upgrade regardless of the currently
installed version, run `idf.py force-upload` or `idf.py force-upload-config`
respectively.

## Home Assistant Configuration

Below is an example YAML configuration for adding an air conditioner to Home
Assistant as a climate control device:

```yaml
mqtt:
  climate:
    - name: My AC
      availability:
        - topic: "AC-MITM-XXXX/Status"
          payload_available: "online"
          payload_not_available: "offline"
      action_topic: "AC-MITM-XXXX/Action"
      fan_modes:
        - "auto"
        - "low"
        - "medium"
        - "high"
      fan_mode_state_topic: "AC-MITM-XXXX/Fan"
      fan_mode_command_topic: "AC-MITM-XXXX/Fan/Set"
      min_temp: 16
      max_temp: 30
      modes:
        - "off"
        - "auto"
        - "cool"
        - "heat"
        - "dry"
        - "fan_only"
      mode_command_topic: "AC-MITM-XXXX/Mode/Set"
      mode_state_topic: "AC-MITM-XXXX/Mode"
      power_command_topic: "AC-MITM-XXXX/Power/Set"
      payload_on: "on"
      payload_off: "off"
      current_temperature_topic: "AC-MITM-XXXX/Temperature"
      temperature_state_topic: "AC-MITM-XXXX/Temperature"
      temperature_command_topic: "AC-MITM-XXXX/Temperature/Set"
      temperature_unit: "C"
      precision: 1.0
```

## Board Compatibility

The `sdkconfig.defaults` included in this project covers common configurations.

### Olimex ESP32-POE

A number of minor changes are required to support this board:
* Set the `eth` section as follows:
  ```json
  {
    "network": {
      "eth": {
        "phy": "LAN8720",
        "phy_power_pin": 12
      }
    }
  }
  ```
* Run `idf.py menuconfig` and modify the Ethernet configuration to:
  * RMII_CLK_OUTPUT=y
  * RMII_CLK_OUT_GPIO=17
