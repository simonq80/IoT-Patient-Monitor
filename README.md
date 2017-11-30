# Team-03 Patient Monitor (Proposal)

## Source Code Directory

Please follow the readme.md to get your build env setup
```
/zephyr/cs7ns2/patient_monitor/
```

### Approach:
1. Monitor when patients are sleeping
2. Use cloud service to schedule patient wake times.
3. Ring alarm to wake patient based on system patient schedule (actuation) (example: Wake patient for breakfast/lunch)
4. Using collected patient data, turn lights off when patients are asleep/ turn on when awake (actuation)
5. Send sleep data to cloud to analyze patient sleep/wake cycles.

### Sensors:
* Gyro / Pressure Sensor (Movement of patient) `possibily another sensor`

### Output:
* Speaker
* Lights

### Requirements:
* CRUD app (Patient Schedule)
* I/O device connection
* MTTQ protocol handlers (Python)
* Configure Things-Board Broker
