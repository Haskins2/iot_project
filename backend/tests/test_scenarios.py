import paho.mqtt.client as mqtt, ssl, json, time

c = mqtt.Client(client_id='test-pub')
c.tls_set(ca_certs='/certs/ca.crt', certfile='/certs/actuation-service.crt', keyfile='/certs/actuation-service.key')
c.tls_insecure_set(True)
c.connect('mosquitto', 8883, 60)
c.loop_start()
time.sleep(2)

def pub(device, level, analog, digital):
    msg = json.dumps({'water_level': level, 'raindrop_sensor': {'analog': analog, 'digital': digital}, 'ts': '2026-03-30T14:25:00Z'})
    c.publish(f'devices/{device}/water_level', msg, qos=1)
    rain = 'RAIN' if (digital == 1 or analog >= 1500) else 'dry'
    print(f'  {device}: water={level}  rain={rain} (analog={analog} digital={digital})')
    time.sleep(1.2)

# ========================================================
# TEST 1: HYSTERESIS  (device: hysteresis-test)
# ON threshold = 1900, OFF threshold = 1880
# Pump should NOT flip-flop in the 1880-1900 dead zone
# ========================================================
print('=== TEST 1: HYSTERESIS ===')

print('Step 1: Start low (pump should stay OFF)')
pub('hysteresis-test', 1400, 400, 0)
pub('hysteresis-test', 1600, 400, 0)

print('Step 2: Enter dead zone 1880-1900 (pump should stay OFF - never activated)')
pub('hysteresis-test', 1890, 400, 0)
pub('hysteresis-test', 1895, 400, 0)

print('Step 3: Cross ON threshold with rain (pump ACTIVATES)')
pub('hysteresis-test', 1920, 1600, 1)
pub('hysteresis-test', 1950, 1700, 1)
pub('hysteresis-test', 1910, 1600, 1)

print('Step 4: Drop INTO dead zone (pump should STAY ON - hysteresis)')
pub('hysteresis-test', 1890, 1500, 1)
pub('hysteresis-test', 1885, 1500, 1)

print('Step 5: Drop BELOW 1880 (pump DEACTIVATES)')
pub('hysteresis-test', 1870, 400, 0)
pub('hysteresis-test', 1850, 400, 0)

# ========================================================
# TEST 2: RAINDROP CONFIRMATION  (device: rain-test)
# High water WITHOUT rain should NOT activate pump
# High water WITH rain SHOULD activate pump
# ========================================================
print('')
print('=== TEST 2: RAINDROP CONFIRMATION ===')

print('Step 1: High water but NO rain (pump should NOT activate)')
pub('rain-test', 1950, 400, 0)
pub('rain-test', 1960, 300, 0)
pub('rain-test', 1970, 200, 0)

print('Step 2: High water WITH rain via digital=1 (pump ACTIVATES)')
pub('rain-test', 1950, 400, 1)
pub('rain-test', 1960, 500, 1)

print('Step 3: Drop to normal (pump deactivates)')
pub('rain-test', 1500, 300, 0)

# ========================================================
# TEST 3: RAIN via ANALOG only  (device: analog-rain-test)
# digital=0 but analog >= 1500 should count as rain
# ========================================================
print('')
print('=== TEST 3: ANALOG RAIN DETECTION ===')

print('Step 1: Baseline readings')
pub('analog-rain-test', 1400, 400, 0)
pub('analog-rain-test', 1600, 500, 0)

print('Step 2: High water + analog rain only (digital stays 0)')
pub('analog-rain-test', 1920, 1600, 0)
pub('analog-rain-test', 1940, 1700, 0)
pub('analog-rain-test', 1950, 1550, 0)

print('Step 3: Back to normal')
pub('analog-rain-test', 1300, 400, 0)

print('')
print('Done - all 3 test scenarios sent')
c.disconnect()
